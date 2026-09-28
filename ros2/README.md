# ros2/ — DMP ↔ ROS2 遥测桥

把 DMP 已有的**文本网关**（`gateway_service`，TCP:9100）接成 ROS2 的一等公民数据源：
上游是 ROS2 世界（Nav2、`ros2_control`、`diagnostic_aggregator`、rviz），下游是既有采集/告警链路，
**桥不改任何测量语义，也不进控制回路**。

```
STM32/模拟器 ──帧协议──> gateway_service ──RAW 字节流──> bridge_node ──> /dmp/frames (保真层)
                                          └──文本行 API──>                /diagnostics (标准视图)
                                                                          /dmp/rules   (锁存配置)
                                                                          三个服务 (只读诊断 + 阈值下发)
```

## 包

| 包 | 类型 | 职责 |
|---|---|---|
| `dmp_msgs` | rosidl 接口 | 只定义契约：`DeviceFrame`/`DeviceFrameArray`/`Rule`/`RuleList` + `Selftest`/`SetRule`/`GetRules` |
| `dmp_ros2_bridge` | rclcpp 节点 | 网关客户端（POSIX，无 ROS 依赖的静态库）+ 桥节点 + 纯函数层单测 |

帧解码**不重新实现**：直接 `#include "dmp/frame_protocol.h"`（header-only），于是
「固件 C 内核 ↔ 上位机 C++ ↔ ROS2 节点」三方共用同一份协议实现。
注意口径：`test_frame_parity` 的 41 项逐字节断言只编译固件 C 与上位机 C++，**并不编译 ROS 节点**；
桥侧的一致性是靠"共用同一份头文件"传递得到的，不是靠那个测试直接验的——这两件事不该混说。

## 接口契约

| 名字 | 类型 | QoS | 理由 |
|---|---|---|---|
| `/dmp/frames` | `dmp_msgs/DeviceFrameArray` | `sensor`（best_effort，KeepLast 20） | 5 Hz×N 通道的高频遥测：宁可丢旧样也不阻塞读线程 |
| `/diagnostics` | `diagnostic_msgs/DiagnosticArray` | reliable，KeepLast 10 | 标准接口，直接喂给 `diagnostic_aggregator`；不重复造轮子 |
| `/dmp/rules` | `dmp_msgs/RuleList` | reliable + `transient_local`，KeepLast 1 | 阈值是**状态**不是事件，晚起的订阅者也当立刻看到当前值 |
| `<prefix>/<device>/selftest` | `dmp_msgs/Selftest` | — | 只读：STATS+ALARMS+RULES 聚合，绝不写 |
| `<prefix>/<device>/set_rule` | `dmp_msgs/SetRule` | — | 写：只改告警判定阈值，见下节安全边界 |
| `<prefix>/<device>/get_rules` | `dmp_msgs/GetRules` | — | 回读服务端**实际生效值**，不是调用方声称值 |

## 三步复现（Linux / VM）

```bash
# 0) Windows 侧打包完整历史（不要求目标机能访问 GitHub）
git bundle create ../build_tmp/dmp.bundle main

# 1) 原生构建 + ctest（核心 5 个测试）
bash tools/vm_native_build.sh

# 2) colcon 构建两个包 + 桥纯函数层断言
bash tools/vm_ros2_build.sh

# 3) 全链路冒烟：simulator → gateway → bridge → topic/service
bash tools/vm_e2e.sh

# 4) QoS 错配四格矩阵取证
bash tools/vm_qos_mismatch.sh
```

`tools/vm_*.sh` 一律不用 `set -e`：这些脚本的价值在于**把每一步真实结果都打出来**，
中途失败时诊断输出比早退更重要。

## 实测结果（2026-09-29，Ubuntu 22.04 / Humble / g++ 11.4）

- 原生 ctest **5/5 通过**，断言 27+36+41+9+58 = **171 项**
- 桥纯函数层 `colcon test` **1 test / 0 failures**，断言 **62 项**（合计 **233**）
- 端到端：`STATS ok=396 crc_err=0 dropped=0`；`/dmp/frames` 实测 `TEMP 37.04 / HR 75.85`；
  `/diagnostics` 每通道一条 `DiagnosticStatus` 且带 `rule` 键值；
  `set_rule{kind:2,low:45,high:115}` → `accepted=True`，**网关侧独立回读** `RULES` 显示 HR 已是 `[45.000, 115.000]`；
  倒置区间 `{low:90,high:20}` → `accepted=False reason='need low < high'`

## QoS 事故复盘：我原来的假设是反的

设计时我认定「发布 reliable + 订阅 sensor(best_effort) 会连得上收不到」，并把 `frame_qos` 留成参数以便复现。
实测四格矩阵（`tools/vm_qos_mismatch.sh`，每格 8 秒窗口，统计帧行数）：

| 发布端 | 订阅端 | 实测收到 | 结论 |
|---|---|---|---|
| BEST_EFFORT | reliable | **0 条** | 不兼容，静默失效 |
| BEST_EFFORT | best_effort | 128 条 | 正常 |
| RELIABLE | best_effort | 136 条 | **兼容**（只是不重传） |
| RELIABLE | reliable | 132 条 | 正常 |

`ros2 topic info /dmp/frames --verbose` 证实发布端 QoS 确实是参数所设的那个值——所以
0 条不是配置没生效，而是**我的兼容矩阵方向记反了**：reliability 的匹配规则是
「发布端能力 ⊇ 订阅端要求」，发布端承诺重传时，要求低的订阅端照样收；反过来则一个都收不到。

教训固化为两条：

1. **文档化之前先实测**。"演示点"写在注释里当设计意图，一旦方向错就是误导后来人——源码里那两处注释已按实测改正。
2. **取证手段本身要可信**。第一次跑出的"0 条"其实是被 `timeout` 杀掉的 python 没 flush（块缓冲整块丢失）
   造成的假象。改用 `timeout -s INT`（触发 rclpy 优雅退出并 flush）后，A1 的 0 与 A2 的 128 才同时成立。
   同理，`timeout 2 head -N <&3` 读网关文本应答也会因 `head` 被杀而**看起来"网关没回应"**——
   `vm_e2e.sh` 的 `gwcmd()` 因此改成显式收包的 python 客户端，并在注释里钉死这个坑。

## 本次最贵的一个跨平台 bug：`select()` 的 nfds

现象是「数据面连接正常、`STATS ok=0`、`/dmp/frames` 一条都没有」——链路每一环都不报错。

根因在 `src/device_simulator.cpp`：

```cpp
select(0, &rf, nullptr, nullptr, &tv)   // Windows 忽略首参, 跑得好好的
```

POSIX 的 `nfds` 必须是 **max_fd + 1**，传 0 等于"一个描述符都不检查"，于是
`accept()` 永不发生；而三次握手由内核 `listen` backlog 完成，所以 `connect()` 成功、
`dataUp()==true`、字节数为 0。**Windows 套接字 API 与 POSIX 的这个语义差异，
是"移植即失效"最典型的形状**：现象离根因极远（ROS 侧没数据 → 网关 → 模拟器）。

修法用 `#ifdef _WIN32` 把两种语义显式写开，并在注释里保留症状描述。

## 安全边界（为什么下行只有阈值下发）

`set_rule` 写的是**判定层**（`Acquisition::setRule` → 告警阈值），不写测量链路、不写执行器：

- 网关本身是纯只读的（蜂鸣/告警确认都在 Qt 端），要下发"停机/蜂鸣"就必须改固件——那是另一条风险等级
- 阈值下发可被**回读核对**（`get_rules` 回显服务端实际生效值），且非法区间被拒（`low >= high`、非有限数、类型越界）
- 配置面与数据面走**两条独立 TCP 连接**：同一连接混用 `RAW` 二进制透传与文本应答会导致应答字节插进帧流里无法定界——这是正确性问题，不是风格问题
- 严格浮点解析：`std::atof("abc")` 静默返回 `0.0`，会把畸形配置行当成"低边界为 0"的真实规则。
  `parseFloatStrict` 用 `strtod` + `endptr` 全 token 校验 + `isfinite` 拒之（62 项断言里有一条专门抓它）

安全分级的这一"配置类写入 vs 只读诊断"差异，正是 IEC 62304 语境下值得演一遍的东西，
详见 `docs/adr/ADR-002-下行只写判定层.md`。

## 已知取舍

| 取舍 | 现状 | 更彻底的做法 |
|---|---|---|
| 多行文本应答定界 | 「期望前缀 + 静默期 `quietMs`」组合判定结束 | 网关协议加 `END` 帧（要动协议版本，暂不做） |
| 桥不落库 | 落库/回放留在上位机侧 | 若要在 ROS 侧回放，用 `rosbag2` 而不是桥里再造一套存储 |
| 每设备一节点一前缀 | 机队扩展不改代码，`launch` 传 `device_id` | 未来接 `lifecycle` 做受控启停（见路线图） |
| 丢弃计数分两处 | `bridge_dropped`（本桥缓冲溢出）与网关 `dropped` 分开 | 合并会掩盖是哪一段丢的，故意不合并 |

## 下一步

- `rclcpp::Node` → `LifecycleNode` + component composition（受控启停、零拷贝 intra-process）
- 跨机接真板：Windows 侧串口网关 → VM 桥（先验防火墙与 COM4 占用）
- `/dmp/frames` → `topic_to_ptp`/`pose_broadcaster` 一类的下游联动
