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

## 复现步骤（Linux / VM）

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

# 5) 跨机真板验收（需先在 Windows 侧起 gateway_service --serial COM4）
bash tools/vm_realdevice_e2e.sh 192.168.109.1 9100 bed01

# 6) 生命周期三层负例 + 组件容器 A/B（需要 bridge_node 与组件 .so 都已 install）
bash tools/vm_lifecycle_compose.sh
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

## 跨机真板验收（2026-09-29，STM32 → Windows 网关 → VM 内 ROS2）

数据流：`STM32(USART1) → COM4 → gateway_service.exe 9100 --serial COM4 → (TCP 跨机) → VM 内 dmp_bridge → ROS2`

| 检查项 | 实测 |
|---|---|
| 网关侧（不经 ROS，先排除变量） | `frames_3s=14 seq=[12806..12819]`、`STATS ok=2874 crc_err=0 dropped=0` |
| `/dmp/frames` | `seq_lines=33`，值为板上真实采样（心率 66.7、血氧 97.3、浓度 7.4、体温 37.0） |
| `/diagnostics` | `decoded_ok=89`、`crc_err=0`、`telemetry_link: stream clean` |
| 桥侧发布 QoS | `Reliability: BEST_EFFORT`（`topic info --verbose` 实测，非文档推断） |
| 下行写阈值 | `accepted=True reason='applied'` → `get_rules` HR=`[40.0,105.0]` → **网关侧独立回读** `RULE type=2 name=HR low=40.000 high=105.000 msg=realdevice test` |
| 非法写 | `accepted=False reason='need low < high'` |
| 自检 | `healthy=True ok=3034 crc_err=0 dropped=0 rule_count=3 recent_alarms=5 data_plane=up` |

两个当时没预判到、但值得记下来的事实：

1. **防火墙无需改**。VM 能主动探测到 `192.168.109.1:9100` 可连通（先测后做，不是盲目加规则）。
2. **服务名带设备段**：话题是 `/dmp/frames`，服务却是 `/dmp/bed01/set_rule`。
   写成 `/dmp/set_rule` 只会得一句 `waiting for service to become available...`，**不报错、只卡超时**。

验收脚本会**自己把现场还原**（末尾恢复 HR 默认区间）——而“还原”必须在 kill 桥之前，
因为服务提供方就是桥自己。

## 生命周期与组件容器（c8 + c13~c17）：契约验了，性能理由先被自己推翻、再以窄的形式立回来

桥现在是 `LifecycleNode`，并可以组件形式装进容器。**默认不 autostart**：

```bash
ros2 lifecycle set /dmp_bridge_bed01 configure   # -> inactive: 话题/服务存在, 零数据
ros2 lifecycle set /dmp_bridge_bed01 activate    # -> active:    数据面与判定链路启动
ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 mt:=true
# 组合场景的状态转换**只能外部驱动**（Humble 的 ChangeState 匹配不到 dlopen 出来的组件）
```

三个阶段各自可断言（`tools/vm_lifecycle_compose.sh` 的 [1] 段，c8 当时 **PASS=10 FAIL=0**；
脚本后来又长了 [6] 毒化 A/B、[7] 错误隔离与 [8] `on_shutdown` 三段）：

**整轮断言数（2026-09-29 VM 实跑，不是推算）**：`POISON=1` 完整轮 **PASS=25 FAIL=0**，
构成 [1]3 + [2]3 + [3]2 + [4]2 + [6]4 + [7]5 + [8]6 = 25；`POISON=0` 快档按设计跳过 [6][7]，
同一套脚本给出 **PASS=16 FAIL=0**（10 + [8]6）。两档的原始读数都在
`tools/diagnostics/evidence/c11_full_poison1.txt` / `c11_shutdown_poison0.txt`。
原始日志见本节末「[8] 实跑结果」。

| 阶段 | 话题/服务 | 数据 | 写路径 |
|---|---|---|---|
| unconfigured | 都不存在（`change_state` 除外） | — | 服务不存在，不是"拒答" |
| inactive | 都在 | 6s **0 批** | `accepted=False reason='bridge not active (state=inactive)'`；`get_rules` 仍可读 |
| active | 都在 | 6s **30 批**（最大间隔 250ms） | `accepted=True` |
| cleanup 后 | `/dmp/frames` 从图上消失 | — | 这是 publisher 真被释放的唯一外部证据 |

> 表里那三条"不存在 / 消失"的断言（unconfigured 与 cleanup 后）**不能读 `ros2 topic list` 的默认路径**：
> 那个读的是 ros2-daemon 的图缓存，它对"没来得及道别"就消失的参与者会留条目，而留的时长
> 比轮次间隔长。实测：一轮跑完、进程全没之后 +0s，**现场=0 / daemon=1**，+15s 后两路才同时归零 ——
> 所以这个脚本早几轮在第 2~4 轮报的那次 `实际=1 期望=0` 一直是**假红**（读到的是上一轮的残留条目，
> 首轮必绿只是因为前面没人）。现在图上的断言一律 `--no-daemon --spin-time 6`（现场发现），
> 并把两路读数都打进日志（不一致本身就是信息）。推导与自我推翻的两层见素材录 B-40 / B-42。

两个当时想错的地方（完整推导见 `docs/adr/ADR-003` 与素材录 §7）：

1. **`on_activate` 必须链回基类**。Humble 的 `LifecycleNode` 只实现了 `on_activate`/`on_deactivate`
   两个 override，职责就是遍历 `add_managed_entity()` 登记的发布器翻开关。override 之后不链回去：
   状态到 `active`、话题在、定时器在跑、`publish()` 全被丢——只留一句 logger 名为
   `LifecyclePublisher`（不是节点名）的 WARN。一个根因同时解释两个"零数据"。
2. **"头阻塞"没测出来**。同容器两台设备，把 bed01 控制面打到持续排队（~200 次调用、平均往返 650ms），
   bed02 帧最大间隔基本不变（典型 212ms → 250ms，自然节律就≈200ms）；并发窗口 8→32 仍无差别。
   怕的是"两轮不足以归因"，所以把同一个脚本**不改一行**重复到 **7 轮**：两臂"打满−空载"差值的
   **符号在轮次间翻转**（各 4 正 3 负，中位数 mt +31.2ms / st +15.5ms），14 个打满窗口共 **1 次** >400ms 停顿，
   而那次落在**应当免疫的 mt 臂**（方向与假设相反 → 读作抖动）。最硬的一条是：中位数的**大小关系
   也随样本集合翻转**（6 轮时 mt < st，7 轮时 mt > st）——能翻转的排序就不是效应。**mt 与 st 无可测差异**，
   两个回调组因此从"性能必需"降级为"便宜的保险"；真正的控制点是"任何单个回调都得短"（现最长≈90ms）。
   重复过程中暴露的两个测量学坑（素材录 B-35/B-36）：写死在脚本里的结论行会在数据反对它时照样打印；
   而 `st` 的空载基线就常常高于 `mt` 的打满值 —— 跨臂比不得，只能同臂内比。

这一条到 c13 为止只到"约定"为止：我说了回调得短，但没东西保证它短。下面把它做成实现，
并且顺手把上一轮没覆盖的那条边界量出数字。

### 单个长回调：头阻塞是真的，而且触发条件比我原来说的窄（c13~c17）

上一轮的负结果只证伪了"排队的短回调会饿死同容器"，碰不到"单个回调长"——因为当时 `gateway_host`
全设备共用一个参数，我没法只让**一台**掉进连接黑洞。补上 `gw_overrides` 之后长回调不用人造：
`connectNew()` 原本是阻塞 `connect()` 且没有超时，而它就在调用方的回调里跑（`tickLink` 定时器、
三个服务、以及 `on_configure` 里的 `refreshRules`）。因此本轮两件事一起做：
launch 层支持 per-device 端点覆盖（桥代码零改动），`connect` 改成非阻塞 + `poll` 到绝对 deadline，
上限参数 `connect_timeout_ms`（默认 1000；**0 是故意合法的旧行为档**，专门留给取证脚本做"修与不修"对照）。

毒化目标一律**现场量**而不是写死：用裸 socket 不设超时量出真实阻塞时长，再按它归档——
本机无监听 = 0ms（对照组）、同网段不存在主机 ≈3.06s、私网黑洞地址 ≈21.05s（地址由
`ip -4 route get` 反推子网生成，换网络就变，所以不能写）。每格先只激活 bed02 取**同容器配对基线**。

| 档 | 容器 | `connect_timeout_ms` | 基线→受毒最大间隔 | 速率比 | 批速（批/秒） | 超时计数 |
|---|---|---|---|---|---|---|
| mid | st | 0（旧行为） | 200.9 → **6228.8** ms | **20.0×** | 5.00→0.25 | — |
| mid | mt | 0 | 250.0 → 250.1 ms | **1.0×** | 5.00→5.00 | — |
| mid | st | 1000 | 219.3 → 2002.1 ms | 3.0× | 5.00→1.64 | 26 |
| mid | mt | 1000 | 200.8 → 274.3 ms | 1.0× | 5.00→5.00 | 25 |
| worst | st | 0 | 200.5 → 只剩 1 批（gap 无定义） | **236×** | 5.12→0.02 | 占到读不到状态 |
| worst | st | 1000 | 250.1 → 2003.6 ms | 5.5× | 5.00→0.91 | 51 |
| fast | st | 0 | 281.0 → 302.4 ms | **1.0×** | 5.00→5.00 | 0 |
| fast | mt | 1000 | 250.4 → 259.8 ms | **1.0×** | 5.00→5.00 | 0 |

表全部取**同一轮**（第 1 轮，也是唯一跑了 worst 档的那一轮；21s 那一格成本太高，只跑了一次）。
数字与 `docs/adr/ADR-003` 末节之二的表逐字一致 —— 两份文档里同一格给两个不同的数，
读起来就是有一份在骗人。

四个读数（mid 档重复 4 轮，受毒间隔 6228.8 / 6227.0 / 6226.7 / 6228.8 ms —— 4 轮只差 2.1ms）：

1. **头阻塞存在，但条件从"控制面繁忙"收窄为"单个回调长"**：`st` 20.0×，`mt` 在同样的毒化下 1.0× 完全免疫。
2. **fast 那两格是全表最值钱的**：同样是"网关连不上"，只差拒绝得快（0ms），两容器都 1.0×。
   它把归因钉在"阻塞时长"这一个变量上，而不是"连不上"这件事上——没有它，上面所有数字都可以被
   反驳成"你测的其实是网络故障本身"。
3. **236× 那一格的 `max_gap` 本来无定义**（窗口里只剩 1 批），而脚本一度把无定义打印成 `0.0ms`，
   方向正好反了（素材录 B-37）；主统计量因此换成按各自窗口秒数归一的**批数速率**。
   同一轮里连 `ros2 lifecycle get` 都被 21s 回调饿死——判"实验是否成立"的那个请求本身就是被饿死的
   请求（B-38），所以那一格的合法读数是"占到无法应答查询"，而不是"实验无效"。
4. **限时是封顶而不是消除**：20.0× → 3.0×、236× → 5.5×。残余间隔≈两个上限（相邻两个 tick 各撞一次 1s），
   与实测 2.0s 吻合；mid 档的 6.23s 也正好是 2×3.09s——同一个机制解释两个数字，这是它们可信的原因。

### [7] 组件错误隔离（R-008 的姊妹项，五条断言全过）

`gw_overrides:=bed01=127.0.0.1:0` 恰好是一条**真**的每设备 configure FAILURE（端口 0 是 C++ 侧本来就拒的值），
不需要人造故障。实测：bed01 被拒后停在 `unconfigured`（不是一路推到 active）、容器进程还在、
它自己的三个服务因 `configure` 没走完而不存在、bed02 的 `change_state` 仍在且能激活、另一台照发 40 批。
结论：一个组件配置失败不传染同容器其它组件，也不拆容器。

### [8] `on_shutdown`：转换回调与进程退出是两条不同的路

这一格原本是 ADR-003 里唯一挂着"未覆盖"的边界。要验它得先看清它是**两件事**，而它们的代码路径根本不重合：

- `on_shutdown()` 只在生命周期 SHUTDOWN 转换里被调；它做的事是 `timers_.clear()` + 网关 `closeAll()`。
- SIGINT 走的是另一条：`SingleThreadedExecutor::spin()` 返回 → `rclcpp::shutdown()` → `return 0`，
  **一句 `on_shutdown` 都不经过**。所以"测了进程能退"绝不等于"回调验过了"。

六条断言按这两条路分开设（直接起可执行文件而不是 `ros2 launch`，才能拿到**节点自己的**退出码）：
阳性对照（shutdown 前 4s 窗口确实在发，否则后面那句"零批"永远为真）→ active→shutdown 后状态 `finalized`
→ 日志里有回调那一句 → 5s 窗口数据面 0 批 → SIGINT 后 15s 内自行退出（不退就是悬挂线程，
网关读线程没 join 干净正是这个形状）→ 退出码为 0。

一个设计上得写清的点：`active->shutdown` 直跳 `finalized` 时话题仍在图上（publisher 对象要到
`on_cleanup` 才析构），所以本段不断言图，只落在回调 + 数据面 + 退出码上。

**实跑结果（2026-09-29，VM `HEAD=c14f735`，两档各一次，六条全绿）**：

| 断言 | 期望 | POISON=1 | POISON=0 |
|---|---|---|---|
| 阳性对照：shutdown 前确实在发 | >10 批 | PASS（20 批） | PASS（20 批） |
| active→shutdown 后状态 | `finalized` | PASS | PASS |
| `on_shutdown` 回调真跑了（日志有「桥已静默」） | 1 次 | PASS | PASS |
| shutdown 后数据面静默 | 0 批 | PASS | PASS |
| SIGINT 后 15s 内自行退出 | 未退=0 | PASS | PASS |
| 退出码 | 0 | PASS | PASS |

两档的六个读数完全一致，这本身就是想要的效果：[8] 只依赖单个桥进程与 `ros2 run` 侧的可执行文件，
不依赖 [6][7] 的容器档位。阳性对照那条尤其不能省——没有它，「shutdown 后 0 批」是一条永远为真的断言
（素材录 B-27）。原始输出：`tools/diagnostics/evidence/c11_full_poison1.txt`、`c11_shutdown_poison0.txt`。

## QoS 事故复盘：我原来的假设是反的

> 本文件只记结论；每条 bug 的“我当时误判成什么”统一收到 `docs/经典Bug素材录.md`。

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
| 每设备一节点一前缀 | 机队扩展不改代码，`launch` 传 `device_id`；也可装进一个容器（`dmp_bridge_composed.launch.py`） | ✅ 已接 lifecycle（受控启停）；但 mt 容器的**延迟收益本轮未测出**，见上节 |
| 丢弃计数分两处 | `bridge_dropped`（本桥缓冲溢出）与网关 `dropped` 分开 | 合并会掩盖是哪一段丢的，故意不合并 |

## 下一步

- **intra-process 通信**（本轮没做也不吹）：同容器内的 `/dmp/frames` 仍走 DDS，组件化只省下
  “每设备一份进程 + 一份 DDS 参与者”那笔固定开销。开 `use_intra_process_comms` 后，
  先用本轮同一套可证伪判据（基线自然节律 + 停顿计数）量它到底省不省，**不拿它当默认答案**。
- **网关端点可按设备覆盖**：✅ 已做——`launch` 的 `gw_overrides`（格式 `bed01=host:port`）把毒化变量做成了可控量，
  “某一台连接黑洞 ⇒ 单个回调阻塞数秒”正是靠它量出 mid 档 20.0× 的（见上面的 [6] 节）。
- **错误隔离**：✅ 已验（见上面的 [7] 节，五条断言）。
- **`on_shutdown`**：✅ 已验（见上面的 [8] 节，六条断言两档各一次）——ADR-003 原本挂着“未覆盖”的最后一条边界已闭合。
- `/dmp/frames` → `topic_to_ptp`/`pose_broadcaster` 一类的下游联动
