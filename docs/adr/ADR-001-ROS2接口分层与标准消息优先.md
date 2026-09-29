# ADR-001：ROS2 接口分层——自定义保真消息 + 标准诊断视图

- **状态**：Accepted（2026-09-29，已在 Ubuntu 22.04 / ROS2 Humble 实机验证）
- **相关**：`ros2/dmp_msgs`、`ros2/dmp_ros2_bridge`、`ros2/README.md`、`include/dmp/frame_protocol.h`

## 背景

DMP 的体征帧有 12 字节定长格式（`AA55|seq|channel|type|value|crc16`）、带帧内 CRC、有 `seq` 回绕与
`crc_err`/`dropped` 流级计数。进入 ROS2 时有三种可选形态：只用标准消息、只用自定义消息、或分层。

关键约束：ROS2 侧的**消费者是两类**。一类是通用工具链（`diagnostic_aggregator`、rviz、rosbag2、未来的
Nav2/`ros2_control` 联动），它们只认标准接口；另一类是需要协议保真度的下游（ seq 断洞检测、CRC 完整性
计数、按 `type` 精确路由），标准消息表达不了。

## 决策

**分两层发布，各层只承担自己那件事：**

1. `dmp_msgs/DeviceFrameArray` → `/dmp/frames`：协议保真层。逐帧带 `seq/channel/kind/kind_name/value`，
   流级 `crc_err`/`dropped` 作为**数组属性**而非单帧属性。QoS 默认 `sensor`（best_effort）。
2. `diagnostic_msgs/DiagnosticArray` → `/diagnostics`：标准视图层。链路健康一条 + 每通道一条
   `DiagnosticStatus`，`level` 用网关下发的阈值判（含 5% 预警带），阈值原文进 `values` 的 `rule` 键值。

`dmp_msgs` 只定义接口、不含实现；桥侧的映射逻辑（`rule_mapping.hpp`）是**不依赖 ROS 的纯函数层**，
62 项断言用普通 g++ 即可编译运行。

## 理由与权衡

- 标准消息优先不是为了"合规好看"，而是**买到生态**：`/diagnostics` 一词不改就能被 aggregator 聚合、
  被 rviz 显示、被 rosbag2 录。自定义消息做不到这点。
  > **本句已被 e1 实测修正（2026-09-29）**：“自定义消息做不到这点”写得太宽。实测把自定义
  > `dmp_msgs/msg/DeviceFrameArray` 直接交给 rosbag2：录 231 批、metadata 认得该类型、`rosbag2_py` 能反序列化、
  > `ros2 bag play` 能回放（`tools/vm_downstream_bag.sh`，12 条断言全绿，读数入库
  > `tools/diagnostics/evidence/e1_bag_run2_green.txt`）。所以准确的分级是按**生态接入的类型**而不是按
  > “标准/自定义”一刀两断：**存储与回放**对 rosidl 注册的自定义类型同样开放（因为它们走同一套类型支持）；
  > 真正需要标准消息的是**现成工具直接理解语义**那一类（`diagnostic_aggregator` 聚合、rviz 显示）。
  > 分层决策本身不变（两层都保留），但**当初的措辞会把后人误导到“自定义消息就是孤岛”上去**，
  > 所以这里就地修正而不是删掉——被推翻的理由比没写过的理由更值得留档（同复盘录 B-33/B-46）。
- 只用标准消息（否决方案 A）会把 seq/CRC 语义塞进 `KeyValue` 字符串里，下游要反解——
  把一个有类型契约的问题变成字符串约定问题，正是我在这个项目里最想避免的。
- 只用自定义消息（否决方案 B）等于在 ROS2 里再造一个私有协议，桥就成了新的孤岛。
- 分层代价是"两个话题要有一致的时间基准"：两层都用节点时钟打 `stamp`，`/diagnostics` 的周期(1s)
  与 `/dmp/frames` 的周期(50ms) 不同，因此**诊断视图不承诺与某一帧一一对应**——这是刻意选择的口径，
  已在代码注释与本 ADR 记录。

## 被否决的替代方案

- **A**：仅 `diagnostic_msgs` + `JsonValue` 承载细节（丢类型契约）
- **B**：仅自定义 `dmp_msgs`（丢生态）
- **C**：把帧直接发成 `std_msgs/UInt8MultiArray` 原始字节，让下游自己解帧（把解码正确性风险外推给每个消费者）

## 影响与验证条件

- 验证证据：`tools/vm_e2e.sh` 实测 `/dmp/frames` 有真实帧、`/diagnostics` 每通道带判级与 `rule`；
  **e1 下游联动**（`tools/vm_downstream_bag.sh`，2026-09-29 VM 实跑 `PASS=12 FAIL=0 SKIP=1`）把“能被生态消费”
  从理由变成了实测：rosbag2 录/取/回放自定义消息与实时侧逐字段一致。
- **失效条件**：若下游只需要"是否在范围"而不需要 seq/CRC，则保真层可以退役，只留标准视图；
  若引入 ISO 11073 / MDIB 等医疗设备本体，保真层应被其替代而非并存。
- 遗留：帧 `stamp` 目前用接收侧时钟（ms 精度），未做时区/单调钟区分；跨机接真板（路线图）时需要重审。
- 遗留（e1 后更新）：本 ADR 许下的“买到生态”三件事只做了**一件半**——rosbag2（录/取/回放）**已实测**；
  `diagnostic_aggregator` **仍未实跑**（VM 上 sudo 需密码，包未装，验收脚本里是显式 SKIP，不计入通过）；
  rviz 显示只验到“话题可被 `ros2 topic echo` 类工具读懂”，没做图形化验收。这三格的差别止于安装与人力，
  不在机制 —— 但写文档时不能把“机制上可行”当成“已验收”。
