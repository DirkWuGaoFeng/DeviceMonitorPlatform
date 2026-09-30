# DeviceMonitorPlatform — 多仪器数据采集与监控上位机平台

<!-- CI 状态徽章: 指向 DirkWuGaoFeng/DeviceMonitorPlatform, Actions 两条门禁腿都绿后亮起 -->

[![CI](https://github.com/DirkWuGaoFeng/DeviceMonitorPlatform/actions/workflows/ci.yml/badge.svg)](https://github.com/DirkWuGaoFeng/DeviceMonitorPlatform/actions/workflows/ci.yml)

> 一条数据链把 **医疗上位机 / 设备平台 / AI 运维** 三个方向焊在一起：
>
> `[下位机/仪器] ──串口/TCP──> [采集与解码] ──环形缓冲──> [存储+告警] ──> [Qt 上位机实时曲线]`
> 上层再挂 **LLM 运维 Agent**（复用 FiberMaintain/Agent）。

## 这个平台做什么

一句话：**把一台仪器吐出来的二进制字节流，一路做到可看、可存、可告警、可服务化、可被生态工具消费，并且每一步都有能证伪的断言。**

### 链路全景

```
[STM32 固件 firmware/  或  TCP 数据源 device_simulator]
            │  12 字节帧 (帧头/seq/channel/type/value/CRC16)
            ▼
   dmp::Acquisition 解码内核  ── 粘包拆包 · 噪声重同步 · 无锁 SPSC 有界环 · 阈值判定
            │
            ├─► CsvSink / SQLite      落库与回放 (tests/test_storage)
            ├─► monitor_console / monitor_serial   无 Qt 的控制台监控端
            ├─► qt_monitor            Qt6 实时曲线 + 历史回放页 + 声光告警与人工确认
            └─► gateway_service       二进制帧 → 文本行 API (HELP/STATS/ALARMS/HISTORY/RULES/RULE) + RAW 逐字节透传
                     ├─► service/grpc_server      同语义 gRPC 实现 (GetStats/GetAlarms/Subscribe)
                     ├─► ros2/dmp_ros2_bridge     /dmp/frames · /diagnostics · /dmp/rules · 三个服务 (生命周期节点)
                     │       └─► 生态组件消费: rosbag2 录制回放、diagnostic_aggregator 聚合
                     └─► tools/dmp_agent.py       运维 Agent: 把网关命令封装成工具做告警归因
```

### 能力清单（每条都写了它现在被怎么验的）

| 能力                                                                 | 代码在哪                                                            | 验证状态                                                                                                                                              |
| -------------------------------------------------------------------- | ------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------- |
| 帧协议编解码 + CRC16 + 粘包/拆包/噪声重同步                          | `include/dmp/frame_protocol.h`、`firmware/dmp_frame_core.{h,c}` | `test_protocol` 27 断言；固件 C 内核与上位机 C++ **逐字节一致** `test_frame_parity` 41 断言                                                 |
| 无锁 SPSC 环形缓冲 + 有界丢弃计数                                    | `include/dmp/ring_buffer.h`                                       | 含在 27 项内（10 万元素多线程不丢不重）                                                                                                               |
| 采集流水线（解码→缓冲→落库→告警，Sink 抽象）                      | `include/dmp/acquisition.h`                                       | 阈值告警与丢弃计数由`test_storage` 36 断言覆盖                                                                                                      |
| 多线程采集（阻塞读源独立线程）                                       | `include/dmp/pipeline.h`                                          | `test_pipeline` 9 断言：N=5000 帧、每块 7 字节恶意拆包，不丢不重不乱序                                                                              |
| 落库与回放（CSV / SQLite + 时间轴窗口）                              | `include/dmp/storage.h`、`src/qt_monitor.cpp`                   | `test_storage` 36；Qt 侧 `--verify` 50/50（解码=drain=落库增量=曲线对齐）                                                                         |
| 控制台上位机（TCP / 裸串口两种源）                                   | `src/monitor_console.cpp`、`src/monitor_serial.cpp`             | 端到端冒烟 ok 递增、crc_err=0、drop=0                                                                                                                 |
| Qt6 上位机（滚动曲线 + 危急值横幅 + 历史回放 + 声光告警与人工确认）  | `src/qt_monitor.cpp`                                              | 编译链接通过 +`--verify` 无头自检；GUI 交互需桌面会话                                                                                               |
| 遥测网关（1 上游 → N 客户端扇出，文本 API + RAW 透传）              | `src/gateway_service.cpp`、`include/dmp/service_proto.h`        | `test_service_proto` 58 断言 + `gw_probe.ps1` 手验；**已知未消减缺陷 R-009**（慢客户端可卡死主循环，见 `docs/网关与服务链路优化方案.md`） |
| gRPC 服务层（同语义第二实现）                                        | `proto/telemetry.proto`、`service/`                             | WSL2 内编成真服务；真机全链路`e2e_realdevice.sh`                                                                                                    |
| ROS2 桥（保真话题 + 标准诊断视图 + 只写判定层的配置服务 + 生命周期） | `ros2/dmp_ros2_bridge`、`ros2/dmp_msgs`                         | 桥纯函数层 62 断言；VM 实跑：生命周期验收**25 条**、下游联动 **22 条** `PASS=22 FAIL=0 SKIP=0`                                          |
| LLM 运维 Agent（自然语言→告警归因）                                 | `tools/dmp_agent.py`                                              | 离线确定性模式 CI 可跑；真机实跑给出归因结论                                                                                                          |
| 合规与可追溯（IEC 62304 / ISO 14971）                                | `docs/IEC62304-软件生命周期与风险管理.md`                         | SR-001~013 双向闭环；SR-014~017 与 R-009 **待实施**，不计入全绿口径                                                                            |
| 质量门禁                                                             | `.github/workflows/ci.yml`                                        | Linux gcc/clang + Windows MinGW + Linux Qt6 腿，`ctest` 跑 171 项核心断言                                                                           |

## 目录结构

```
DeviceMonitorPlatform/
├── CMakeLists.txt              # 顶层构建 (核心/测试/可选 Qt; 已启用 C 语言以编译固件纯 C 内核)
├── .github/workflows/ci.yml    # 质量门禁: Linux(gcc/clang)+Windows(MSYS2 MinGW) 自动 ctest 171 断言
│                               #   (ROS2 桥的 62 项断言需 Humble 环境, 走 tools/vm_ros2_build.sh, 暂不在 CI 腿内)
├── include/dmp/
│   ├── frame_protocol.h        # 设备帧协议 (帧头/seq/channel/type/value/CRC16 + 粘包拆包)
│   ├── ring_buffer.h           # 无锁 SPSC 环形缓冲 (UI 与采集解耦)
│   ├── acquisition.h           # 采集流水线: 解码→缓冲→落库→告警 (Sink 抽象)
│   ├── storage.h               # CsvSink 落盘 / loadCsv 回放 / TeeSink 扇出
│   ├── pipeline.h              # ProducerThread: 阻塞读源独立线程 + 原子统计快照 (多线程采集)
│   └── service_proto.h         # 网关文本行协议纯函数 (parseCommand/format*, 可单测)
├── proto/
│   └── telemetry.proto         # gRPC 服务契约 (Sample/Alarm/Stats + Subscribe 流), 与文本协议同语义
├── src/
│   ├── device_simulator.cpp    # 充当"被测仪器"的 TCP 数据源
│   ├── monitor_console.cpp     # 纯 C++ 控制台监控端 (TCP, 无 Qt, 多线程采集)
│   ├── monitor_serial.cpp      # 纯 C++ 控制台监控端 (裸串口 COM, 接真实 STM32, 多线程采集)
│   ├── gateway_service.cpp     # 遥测网关服务 (select 单线程, 二进制帧→文本 API, 多客户端订阅)
│   └── qt_monitor.cpp          # Qt6 上位机: 实时曲线 + 历史回放 + 告警声光/确认 + SQLite + TCP/串口
├── service/                    # 真 gRPC 服务层 (WSL2/Linux 侧, apt 版 gRPC)
│   ├── grpc_server.cpp         # Telemetry gRPC server (合成数据源自洽 / 可连上游); GetStats/GetAlarms/Subscribe
│   ├── grpc_client_probe.cpp   # gRPC 客户端探针 (验证三个 rpc)
│   └── CMakeLists.txt          # protoc+grpc_cpp_plugin 代码生成 + 链接 gRPC::grpc++
├── ros2/                       # ROS2 Humble 侧 (colcon 包, 与核心同一仓库同一帧协议)
│   ├── README.md               # 桥的接口契约/实测记录/QoS 事故复盘
│   ├── dmp_msgs/               # rosidl 接口: DeviceFrame(Array)/Rule(List) + Selftest/SetRule/GetRules
│   └── dmp_ros2_bridge/        # 网关侧客户端(POSIX, 无 ROS 依赖) + 桥节点 + 纯函数层断言
├── wsl_setup.sh                # WSL2 一键: apt 装 gRPC 依赖 + 构建 + 合成模式冒烟自检
├── run_demo.ps1                # Windows 一键: 起模拟器→(可选)网关→Qt 自动连 TCP
├── stop_demo.ps1               # 回收上面拉起的后台进程 (支持 -WhatIf 预览)
├── gw_probe.ps1                # 文本网关验证: .NET TcpClient 发 HELP/STATS/ALARMS/SUBSCRIBE
├── deploy_firmware.ps1         # 真机一键: 拷固件+自动打补丁 main.c(USER CODE)+注入 uvprojx(默认 12_usart_printf_hal, 幂等留 .bak)
├── serial_check.ps1            # 真机: 列 COM 口 + (缺则现编) monitor_serial.exe 前台验收串口流
├── run_qt_serial.ps1           # 真机: 一键起 Qt GUI 并启动即连串口(--demo-serial COM4)
├── run_gateway_serial.ps1      # 真机: 起串口遥测网关(COM4→TCP :9100, 文本行 API + RAW 透传)
├── tools/dmp_agent.py          # 运维诊断 Agent: 把网关命令封装为工具, 离线确定性归因 + LLM function-calling 双模式
├── tools/vm_native_build.sh    # Linux 侧从 git bundle 同步 + 原生构建 + ctest
├── tools/vm_ros2_build.sh      # colcon 构建 dmp_msgs/dmp_ros2_bridge + colcon test
├── tools/vm_e2e.sh             # 全链路冒烟: simulator→网关→桥→ROS2 话题/服务
├── tools/vm_qos_mismatch.sh    # QoS 错配四格矩阵取证 (实测推翻了我最初的假设)
├── tools/vm_lifecycle_compose.sh # 生命周期主验收：[1]~[8] 两档（POISON=1 整轮 25 条、快档 16 条）
├── tools/vm_downstream_bag.sh  # 下游联动：拿生态组件当证人（rosbag2 录/取/回放 `/dmp/frames` + `diagnostic_aggregator` 聚合 `/diagnostics`，22 条）
├── tools/diagnostics/          # 取证驱动与工具（run_vm_rounds.ps1 一条链：bundle→同步构建→跑→scp 取证据）+ evidence/ 原始读数
├── tools/vm_realdevice_e2e.sh  # 跨机真板验收: STM32→Windows 网关→VM 内 ROS2 话题/服务
├── e2e_realdevice.sh           # WSL: 真机全链路验收(网关→dmp_grpc_server→probe, 含证据输出)
├── firmware/                   # 下位机侧 (STM32F103 HAL), 与上位机共用帧内核
│   ├── dmp_frame_core.{h,c}    # 纯 C 帧编码内核 (无 HAL 依赖, PC/MCU 同源校验)
│   ├── dmp_frame.{h,c}         # USART1 发送 glue + 合成采样任务 dmp_task()
│   └── README.md               # 如何并入 Keil 工程(推荐 12_usart_printf_hal)
├── tests/
│   ├── test_protocol.cpp       # 协议/环形缓冲 断言
│   ├── test_storage.cpp        # 存储/回放 断言 (含 帧→CSV→回放 集成)
│   ├── test_frame_parity.cpp   # 固件 C 内核 vs 上位机 C++ 逐字节一致性
│   ├── test_pipeline.cpp       # 多线程采集: 跨线程 5000 帧恶意拆包 不丢不重不乱序
│   └── test_service_proto.cpp  # 网关命令解析/格式化 纯函数 58 断言 (含 RULES/RULE 阈值面)
└── docs/
    ├── adr/                    # 架构决策记录: ADR-001 接口分层/标准消息优先, ADR-002 下行只写判定层, ADR-003 桥生命周期化与组件化
    ├── architecture.md         # 架构设计
    ├── IEC62304-软件生命周期与风险管理.md   # 合规文档 + 可追溯矩阵(SR-001~013；SR-014~017 待实施) + 风险分析(R-001~009)
    ├── 网关与服务链路优化方案.md   # DMP-OPT-001 v1.1 已定案、待实施: 慢客户端隔离(R-009) / Qt 经网关取数 / 原生 Linux 形态 / 多设备纳管
    ├── Linux-gRPC-服务层落地指南.md # 在原生 Ubuntu 22.04(VM/工控机) 把 proto 契约编成真 gRPC 服务的分步指南（Linux 侧唯一环境；WSL2 已退役，见 IEC62304 §1）
    ├── grpc-verify-log.md        # gRPC 服务层一次完整构建+端到端自检通过的真实输出(可复现证据)
    ├── 真机链路-STM32到上位机.md  # 固件并入 Keil + 刷录 + 串口验收 + 真机→gRPC 全链路 端到端 bring-up
    ├── 运维Agent-诊断指南.md      # 自然语言→告警归因：Agent 工具化网关命令 + 离线/LLM 双模式
    └── Bug复盘与判断记录.md         # 55 条真实 bug 的复盘（现象→误判→根因→可复用的判断）
```

## 操作方法

三套环境分工写死在此，避免口径漂移（依据 `docs/IEC62304-软件生命周期与风险管理.md` §1）：

| 环境                                        | 在这里做什么                                                            | 不做什么                                    |
| ------------------------------------------- | ----------------------------------------------------------------------- | ------------------------------------------- |
| **Windows 原生** + Qt 6.10.0 mingw_64 | Qt GUI、真机串口、网关演示、一键脚本、VM 取证驱动                       | —                                          |
| **WSL2 Ubuntu**                       | 装 apt 版 gRPC 依赖、构建、合成模式自检 ——**仅构建/工具链沙箱** | 不作为交付运行环境（进不了 POUD/SOUP 论证） |
| **Ubuntu 22.04 VM + ROS2 Humble**     | colcon 构建桥、端到端/生命周期/下游联动/跨机真板验收                    | 不跑 Qt GUI                                 |

想干什么 → 跑哪一节：

| 目的                           | 小节               |
| ------------------------------ | ------------------ |
| 只要核心库，跑 171 项断言      | 核心构建与测试     |
| 看实时曲线 / 无头自检上位机    | Qt6 上位机         |
| 模拟器 → 网关 → 多客户端演示 | 一键演示与网关联调 |
| 用真板子替掉模拟器             | 接真实 STM32       |
| 把服务层做成真 gRPC            | gRPC 服务层        |
| 接进 ROS2 并验收下游           | ROS2 桥与下游验收  |
| 问“3 号通道为何频繁告警”     | 运维诊断 Agent     |

### 核心构建与测试（不依赖 Qt）

```bash
cmake -S . -B build -DDMP_BUILD_QT=OFF
cmake --build build -j
./build/device_simulator            # 终端A: 启动模拟设备 :9000
```

### Qt6 上位机：构建与运行

需安装 Qt6（Core/Gui/Widgets/Charts/Sql/Network/SerialPort）。本机用 **Qt 自带工具链**实测通过（PowerShell）：

```powershell
$Qt='G:/Software/Qt'; $cm="$Qt/Tools/CMake_64/bin/cmake.exe"
& $cm -S . -B build_qt -G Ninja -DCMAKE_BUILD_TYPE=Release -DDMP_BUILD_QT=ON `
   -DCMAKE_PREFIX_PATH="$Qt/6.10.0/mingw_64" `
   -DCMAKE_CXX_COMPILER="$Qt/Tools/mingw1310_64/bin/g++.exe" `
   -DCMAKE_MAKE_PROGRAM="$Qt/Tools/Ninja/ninja.exe"
& $cm --build build_qt --target qt_monitor        # 编译+链接
# 运行 (交互桌面会话):
$env:PATH="$Qt/6.10.0/mingw_64/bin;$Qt/Tools/mingw1310_64/bin;"+$env:PATH
& .\build_qt\qt_monitor.exe --verify              # 同步自检, 打印 ... -> PASS
# 或双击 build_qt\qt_monitor.exe, 选"串口(STM32)"或"TCP 模拟器"后点连接
```

### 一键演示与网关联调

```powershell
.\run_demo.ps1                        # 模拟器(:9000) + Qt 上位机(自动连 TCP)
.\run_demo.ps1 -Gateway               # 中间插一层网关(:9100 <- :9000)，Qt 连网关看订阅流
.\run_demo.ps1 -SimPort 9010          # 换模拟器端口
.\gw_probe.ps1                        # 对网关发 HELP/STATS/ALARMS 后退出（前提: 先跑 -Gateway）
.\gw_probe.ps1 -Subscribe -Seconds 5  # 订阅实时 SAMPLE 推送 5 秒
.\stop_demo.ps1                       # 异常中断后清扫残留进程；加 -WhatIf 只看不动手
```

也可以手搓：`\build\gateway_service.exe 9100 127.0.0.1 9000`，然后 `telnet/nc 127.0.0.1 9100` 手输 `STATS` / `ALARMS 5` / `SUBSCRIBE` / `HELP`。

### 接真实 STM32（把数据源换成板子）

```powershell
.\deploy_firmware.ps1                 # 一键把 firmware/ 并入 Keil 工程(默认 12_usart_printf_hal)
                                      #   拷 4 个源文件 + 按 USER CODE 标记插 3 行 + 注入 2 个编译单元(幂等, 留 .dmp.bak)
# 之后在 Keil 里编译下载
.\serial_check.ps1 -Port COM4         # 列 COM 口 -> 缺则现编 monitor_serial.exe -> 前台读真帧(看 ok 递增、CRC 不误报)
.\run_qt_serial.ps1 -Port COM4        # 起 Qt GUI 并启动即连串口(--demo-serial COM4)
.\run_gateway_serial.ps1 -Port COM4 -Listen 9100   # 起串口遥测网关: 串口 -> TCP :9100 (文本 API + RAW 透传)
```

步骤与排障见 `docs/真机链路-STM32到上位机.md`。注意 COM 口独占，两个脚本会先释放端口再启动（该互斥的成因与消解方案登记在 `docs/网关与服务链路优化方案.md` P4）。

### gRPC 服务层（在 WSL2 里构建，仅作构建沙箱）

```bash
cd /mnt/e/Work/McuProject/DeviceMonitorPlatform
bash wsl_setup.sh            # 装 apt 版 gRPC 依赖(会 sudo) + 构建 + 合成模式冒烟自检
bash wsl_setup.sh --build    # 只构建
bash wsl_setup.sh --run      # 构建后前台启动 server
bash e2e_realdevice.sh <windows_ip>   # 真机全链路: 串口 -> Windows 网关(:9100) -> WSL gRPC(:50051) -> probe
```

分步说明 `docs/Linux-gRPC-服务层落地指南.md`；一次完整通过的真实输出 `docs/grpc-verify-log.md`（§6 为原生 VM 实跑）。

### ROS2 桥与下游验收（Ubuntu 22.04 VM + Humble）

前提：VM 上已 `source /opt/ros/humble/setup.bash`，且已装 `ros-humble-diagnostic-aggregator`（下游联动 [6] 格要用）。远端地址**不写死也不猜测**，走环境变量：

```powershell
# Windows 侧一条链: git bundle -> scp -> 远端同步构建 -> 跑指定脚本 -> 原始读数 scp 取回落文件
$env:DMP_VM_IP='<VM 地址>'; $env:DMP_VM_USER='<用户名>'
.\tools\diagnostics\run_vm_rounds.ps1 -RepoScript tools/vm_e2e.sh                # 全链路冒烟
.\tools\diagnostics\run_vm_rounds.ps1 -RepoScript tools/vm_lifecycle_compose.sh  # 生命周期 + 长回调毒化
.\tools\diagnostics\run_vm_rounds.ps1 -RepoScript tools/vm_downstream_bag.sh     # rosbag2 + diagnostic_aggregator
```

想手动分步（或直接进 VM）：

```bash
git bundle create ../build_tmp/dmp.bundle main        # Windows 侧打包
scp ../build_tmp/dmp.bundle <user>@<vm>:~/
bash tools/vm_native_build.sh      # Linux 原生构建 + ctest
bash tools/vm_ros2_build.sh        # colcon 构建 dmp_msgs / dmp_ros2_bridge + colcon test
bash tools/vm_e2e.sh               # simulator -> gateway_service -> 桥 -> ROS2 话题/服务
bash tools/vm_qos_mismatch.sh      # QoS 错配四格矩阵取证
POISON=1 bash tools/vm_lifecycle_compose.sh   # 整轮 25 条（POISON=0 快档 16 条）
bash tools/vm_downstream_bag.sh    # 下游联动 22 条（rosbag2 录/取/回放 + 聚合器 + 告警传播）
bash tools/vm_realdevice_e2e.sh <windows_ip> 9100 bed01   # 跨机真板: STM32->Windows 网关->VM 内 ROS2
```

接口契约与实测记录在 `ros2/README.md`；取证工具与每份原始读数的对应关系在 `tools/diagnostics/README.md`。

### 运维诊断 Agent

```powershell
python tools/dmp_agent.py "3号通道为何频繁告警?" --collect 10   # 离线确定性归因(无密钥、结果可复现)
# --llm 模式: OpenAI 兼容 /chat/completions + function calling, base_url/model/key 全走环境变量
```

它把网关的 `STATS`/`ALARMS`/`HISTORY` 封装成工具；`HISTORY [ch] [秒]` 是网关为归因专门加的逐通道窗口聚合命令。详见 `docs/运维Agent-诊断指南.md`。

### 附录：不用 CMake，直接 g++ 单文件验证

```bash
g++ -std=c++17 -pthread -Iinclude tests/test_protocol.cpp -o test_protocol && ./test_protocol
g++ -std=c++17 -Iinclude tests/test_storage.cpp -o test_storage && ./test_storage
g++ -std=c++17 -Iinclude src/device_simulator.cpp -lws2_32 -o device_simulator
g++ -std=c++17 -Iinclude src/monitor_console.cpp -lws2_32 -o monitor_console
g++ -std=c++17 -Iinclude src/monitor_serial.cpp -o monitor_serial   # 裸串口接真实 STM32
g++ -std=c++17 -Iinclude -Ifirmware tests/test_frame_parity.cpp firmware/dmp_frame_core.c -o test_frame_parity && ./test_frame_parity
# 终端A ./device_simulator 9000 ; 终端B ./monitor_console 127.0.0.1 9000 out.csv   (第3个参数为导出 CSV)
```

亦可 `cmake -S . -B build && cmake --build build`（Qt 上位机加 `-DDMP_BUILD_QT=ON`）。

## 落地路线（对应 8 周计划 W3-5）

1. [X] 帧协议 + CRC + 粘包拆包
2. [X] 用 **STM32 板** 替换 `device_simulator`：`firmware/` 按同一帧协议经 USART1 发帧 + 上位机裸串口接入。**已实机验收通过**（12_usart_printf_hal 刷录后 raw/ok 同步增长，四路体征实时解码，工具链：`deploy_firmware.ps1` 一键接入 + `serial_check.ps1` 串口验收 + `docs/真机链路-STM32到上位机.md`）；Qt GUI 一键连真机：`run_qt_serial.ps1`（启动即 `--demo-serial COM4`）；无头回归 `qt_monitor --verify` PASS(50/50 解码=落库=曲线对齐)
3. [X] 环形缓冲接入真实多线程采集线程（`pipeline.h` ProducerThread：读源独立线程 + 原子统计快照；串口/控制台均接入）
4. [X] 历史落库 + 回放 + CSV 导出（`storage.h` CSV；Qt 端 SQLite + **历史回放页/时间轴滑动窗**）
5. [X] 危急值阈值告警（控制台横幅 + Qt 告警条 + **声音 `QApplication::beep` + 人工确认按钮**，演练 IEC 62304 C 类）
6. [X] 把采集/告警后端抽象成服务（两种同语义实现：Windows 侧 `gateway_service.cpp` TCP 文本网关；**WSL2 侧 `service/grpc_server.cpp` 真 gRPC**（实现 `proto/telemetry.proto` 的 GetStats/GetAlarms/Subscribe）；共用同一 `Acquisition` 内核，业务层不动只换传输层）。**已串成真机全链路**：STM32→串口网关(RAW 透传)→WSL gRPC→probe，实测 `ok=34 crc_err=0` + 10 条真机样本流式到达（`e2e_realdevice.sh`）
7. [X] 挂 LLM 运维 Agent：“3 号通道为何频繁告警？”（`tools/dmp_agent.py`，纯 stdlib）：网关新增 `HISTORY [ch] [秒]` 逐通道窗口聚合命令供归因；Agent 把 STATS/ALARMS/HISTORY/SUBSCRIBE 封装为**工具**，双模式：离线确定性诊断（无密钥、CI 可跑）+ OpenAI 兼容 function-calling 循环（`--llm`，环境变量指端点）。真机实跑：正确归因“基线均值 74.8 正常、峰值冲 129.6 = 周期性越限尖峰(固件演示注入)，非链路故障”（详见 `docs/运维Agent-诊断指南.md`）
8. [X] 按 `docs/IEC62304-软件生命周期与风险管理.md` 补齐可追溯矩阵与风险分析：安全分级定档（Agent 归 A 类、告警链路 C 类）；风险表 6 项（R-001~006）补全严重度/概率/风险评价/控制措施→验证闭环 + 剩余风险/收益-风险/PMCF 评价；可追溯矩阵扩至 **SR-001~011**（新增 SR-010 真机→gRPC 全链路、SR-011 运维 Agent；SR-005 改真机已实机验收、SR-008 断言 15→25），**171 项核心断言全绿**且需求↔测试双向闭环
9. [X] 挂 **CI 质量门禁**（`.github/workflows/ci.yml`）：Linux gcc Release/Debug 作红绿灯门禁（已本地同构实测），clang 为咨询腿（`continue-on-error`），Windows 走 MSYS2 MinGW；**上 CI 过程中顺手修掉 3 个真可移植性 bug**（详 `docs/IEC62304-软件生命周期与风险管理.md` §6 回归策略）
1. [X] **接入 ROS2 Humble**（`ros2/`）：`dmp_msgs` 定契约，`dmp_ros2_bridge` 把网关的帧流与阈值面接成 `/dmp/frames`（协议保真）+ `/diagnostics`（标准 `diagnostic_msgs` 视图）+ `/dmp/rules`（锁存）+ 三个服务；VM 内实跑端到端（`ok=396 crc_err=0`，`set_rule` 写入后**从网关侧独立回读核对**，非法区间被拒）。产出 `docs/adr/ADR-001/002` + `ros2/README.md` 的 **QoS 实测矩阵**（推翻了设计时“pub reliable + sub best_effort 会静默”的错误假设，真正错的方向是 pub best_effort + sub reliable）；过程中修掉一个最贵的跨平台 bug：`device_simulator` 的 `select(0,...)` 在 POSIX 下等于不检查任何描述符，accept 永不发生（症状：“连接正常、零字节”）
1. [X] **跨机真板接入 ROS2**（`tools/vm_realdevice_e2e.sh`）：STM32→COM4→Windows 网关→（TCP 跨机）→VM 内桥→ROS2。实测 `/dmp/frames` 33 条真帧、`/diagnostics decoded_ok=89 crc_err=0`、`selftest healthy=True ok=3034`；从 ROS 侧 `set_rule` 后**网关侧独立回读**得 `HR low=40.000 high=105.000`，倒置区间 `accepted=False`；脚本末尾自动还原现场
1. [X] **查清 CI 那条 `exit code 1`（1）修掉一个真 bug**：本地直接跑 `qt_monitor --verify` 即复现 `db_rows=358 -> FAIL`。根因是自检拿 `SELECT COUNT(*)` **绝对值**与 N 比，而 `device_history.db` 是跨次运行的持久文件 —— **这个自检从设计上只在空库时成立**。改为增量计数后本地连跑 3 次 PASS（`+50` 恒定）
1. [X] **查清 CI 那条 `exit code 1`（2）结论反转 → 注解锁定根因 → 已修复**：逐步核实后，失败**步骤**是 `Build qt_monitor`（CMake configure 阶段），`Link check` 与 `Headless verify` 都是 **skipped** —— 所以那个 exit 1 **与 `--verify` 无关**（上面那个是独立真 bug，只是排在挂点之后）。Actions 日志正文需登录（REST 端点返 403），于是把该腿改成“失败时用 `::error::` 把 `CMake Error` / `error:` / `undefined reference` grep 成注解”——注解在 Summary 页**匿名可读**，不需要任何人交凭据。**该设计第一次跑就兑现**：Run #4 拿到报错行 `CMake Error at CMakeLists.txt:44 (find_package)`；根因是 `install-qt-action` 的 `modules:` 只点了 `qtcharts`，而 `find_package` 还点名了 `SerialPort`（Qt 模块不随 qtbase 打包；本地是安装向导“全选”所以永远复现不了）。补上 `qtserialport` 后 **Run #5：Qt6 job 8 步全 success、Summary 页 0 errors**（详 `docs/Bug复盘与判断记录.md` B-24/B-25）
1. [X] **桥节点生命周期化 + 组件容器**（`LifecycleNode`、`rclcpp_components`、`launch/dmp_bridge_composed.launch.py`）：把“采集开没开”从“看话题有没有数据”变成**可查询可断言的状态机**——`tools/vm_lifecycle_compose.sh` 三层负例 **PASS=10 FAIL=0**（unconfigured 无话题无服务 / inactive 话题在但 6s 零批且 `set_rule` 被拒并回显 `state=inactive` / active 6s 30 批 / cleanup 后话题从图上消失）。**但我为架构决策设计的 A/B 实验把决策本身推翻了**：控制面饱和（201 次调用/平均往返 650ms）下同容器另一台设备帧最大间隔基本不变（典型 212→250ms），`mt` 与 `st` 无可测差异；为不让“两轮”撑起“停顿全 0”这种全称断言，**同一脚本不改一行重复到 7 轮**：两臂“打满−空载”差值符号在轮次间翻转（各 4 正 3 负，中位数 +31.2 / +15.5ms，且这个大小关系随样本集合还会翻转），14 个打满窗口共 1 次 >400ms 停顿且落在**应当免疫的 mt 臂** → 两个回调组从“性能必需”降级为“便宜的保险”，真实控制点改写为“任何单个回调必须短”（详 `docs/adr/ADR-003`、复盘录 §7 B-26~B-36）。本轮另修掉四个真构建/验证错：`on_activate` 未链回基类导致 publisher 永不激活、OBJECT 库缺 PIC、bundle 缺前置提交造成的“旧代码编译通过”假绿、**写死在验收脚本里的结论行（数据出现反例时它照样打印）**——末句那个“降级”在下一轮又被部分**恢复**了，见第 15 条
1. [X] **把“任何单个回调必须短”从约定做成实现，并量出它到底值多少**（c13~c17）：上一轮那个负结果只证伪了“排队的短回调会饿死同容器”，碰不到“单个回调长”那条边界——因为 `gateway_host` 全设备共用一个参数，我造不出“只有某一台掉进连接黑洞”。补上 `gw_overrides`（launch 层 per-device 端点覆盖，桥代码零改动）后长回调不用人造：`connectNew()` 本来就是无超时的阻塞 `connect()`，而它跑在 `tickLink` 定时器、三个服务、以及 `on_configure` 的 `refreshRules` 里——**一个坏网关地址就能冻住整个单线程容器**。

- **修复**：非阻塞 `connect` + `poll` 到**绝对 deadline**（不是每轮给满额预算，否则“被信号打断就重试”会把口头上的上限变成不是上限），新增 `connect_timeout_ms`（默认 1000；**0 是故意合法的旧行为档**，专门留给取证脚本做“修与不修”对照）。
- **毒化目标现场量而不写死**：裸 socket 不设超时量出真实阻塞时长——本机无监听 0ms（对照组）、同网段不存在主机 ≈3.06s、私网黑洞地址 ≈21.05s（地址由 `ip -4 route get` 反推子网生成，换网络就变）。每格先只激活 bed02 取**同容器配对基线**（上一轮的教训：不同臂之间比不得）。
- **数字**（mid 档重复 4 轮，受毒间隔 6228.8 / 6227.0 / 6226.7 / 6228.8 ms——4 轮只差 2.1ms，因为那是协议栈的 SYN 重试定时而不是噪声）：单线程容器 **20.0×** 而多线程容器 **1.0× 完全免疫**；最狠那一格（21s 黑洞）批速 5.12→0.02 且**连 `ros2 lifecycle get` 都被占得无法应答**；限时后 20.0×→3.0×、236×→5.5×，**封顶而不消除**，残余≈两个上限（相邻两个 tick 各撞一次 1s），与实测 2.0s 吻合。全表最值钱的是 0ms 那两格：同一个“连不上”，只差拒绝得快，两容器都 1.0×——它把归因钉在“阻塞时长”而不是“网络故障本身”上。
- **另修两个把我自己量错的坑**（复盘录 B-37/B-38）：只剩 1 个样本时“最大间隔”本无定义，而脚本把无定义打印成 `0.0ms`，读起来正好是“毫无影响”（主统计量因此换成按窗口秒数归一的批数速率）；以及“判断实验是否成立”的那个状态探针与被试回调共享同一个被耗尽的线程，它自己会先被饿死——于是把“读不到”记成现象（`probe_timeout`）而不是事故（`unknown`）。
- **错误隔离**（R-008 姊妹项）：`gw_overrides:=bed01=127.0.0.1:0` 恰好是一条**真**的每设备 configure FAILURE（端口 0 是 C++ 侧本来就拒的值），不需要人造故障：bed01 停在 `unconfigured`、它自己的三个服务因 configure 没走完而不存在、同容器 bed02 的 `change_state` 仍在且能激活、照发 40 批（复跑轮 41 批）——**一个组件配置失败不传染其它组件，也不拆容器**（该格共 5 条断言）。
- 两个回调组的理由因此以**收窄的形式恢复**：不再是“控制面繁忙会头阻塞”（已被 7 轮 A/B 证伪），而是“单个长回调会头阻塞，多线程容器免疫”。ADR-003 原本挂着“未覆盖”的最后一条边界（`on_shutdown`）本轮已闭合：六条断言两档各跑一次全绿（阳性对照 20 批→状态 `finalized`→回调日志 1 次→数据面 0 批→SIGINT 后 15s 内自退→退出码 0），整轮由 19 项长到 **25 项且实测 PASS=25 FAIL=0**（`POISON=0` 快档按设计跳过 [6][7]，给出 16/16；原始读数入库 `tools/diagnostics/evidence/c11_full_poison1.txt`）。详 `docs/adr/ADR-003` 末节之二、`ros2/README.md`、复盘录 §8 B-37~B-45）
- **顺手把验收尺子也修了一次**（复盘录 B-40~B-42）：`[1]` 那条“unconfigured 时话题必须不存在”在第 2~4 轮各红一次、而首轮独绿——定因为**负断言读了 ros2-daemon 的图缓存**：实测一轮跑完、进程全没之后 +0s 仍是“现场=0 / daemon=1”，+15s 后两路才归零；那个过期窗口罩住了轮次间隔，所以红的那几次读的是**上一轮**的残留条目（而实测到的偏差方向只会造成假红，c8 那 10/10 不被推翻）。图上断言从此一律 `--no-daemon --spin-time 6`，并把两路读数同时打进日志。定因路上我自己错过的两步更值得讲：一个猜测（TERM 杀不掉被长回调占满的容器）被自己的探针**证伪**——TERM 在那种时刻只是延迟生效，退出时仍然道别；另一个更严重：取证探针里那句 `ros2 daemon stop` **把犯罪现场冲干净了**，导致之后连跑 4 轮全绿也不能拿来否证缓存假设——要定因只能用原始配方重跑一次。

1. [X] **下游联动：换一个“不是我写的”证人**（e1+e3，`tools/vm_downstream_bag.sh`，2026-09-29 VM 实跑 **`PASS=22 FAIL=0 SKIP=0`**；e1 那轮是 `PASS=12 SKIP=1`，[6] 刚补上时是 `PASS=19`）：前面那些断言的裁判都是我自己写的订阅方，这轮把 `/dmp/frames` 直接交给生态组件 **rosbag2** 存与回放——实测录到 **231 批**、metadata 认得自定义类型 `dmp_msgs/msg/DeviceFrameArray`，用 `rosbag2_py` 反序列化回来与实时侧**按 `seq` 的窗口逐字段一致**（live 160 / bag 308：找不到 0、字段不同 0），`ros2 bag play` 回放 200 帧同样一致。

- **比较口径不能图省事**：bag 录 16s、实时探针只盯 8s，两个窗长短不等，`cmp -s` 整文件必然红——那是脚本自己的错，不是链路的错。改成“短侧每一行按 `seq` 在长侧都能找到且六字段全同”。
- **回放前必须先把实时流关静默**，而静音**用生命周期门禁**（`deactivate` → 状态 `inactive` → 3s 内 0 帧两条断言）而不是 `kill -INT ros2 launch`：后者实测 15s 未退，跑完还在容器里留下两个孤儿进程（第二个要 `kill -9`）。首轮那格 `play.csv` 混进实时流导致的“200 帧找不到”，是这条前置条件没建立的下游症状——**两格一起红时，只有前一格是事故**。SIGINT 的退出语义由 `vm_lifecycle_compose.sh` 的 [8] 正式断言，不在这里重复。
- **一条我自己下的预设被实测推翻**：按 B-07 的 QoS 四格矩阵，不给 override 直接录应当静默零条，实测 **141/144 批** —— Humble 的 rosbag2 会先查发布端 offered QoS 再自建兼容订阅。所以“错配静默”的准确说法是**订阅端要求高于发布端能力时**，不是“任何下游”。这一格单独计数（`PRED_OVERTURNED`），旧预设与两个读数都留在脚本注释和 `tools/diagnostics/evidence/e1_bag_run1_fail.txt` 里——**改期望值只在能解释之后允许，且不许顺手改成实测值**。
- 新增 [9b] 收尾读数：按名字收干净并量到 `桥 0 个 / sim-gateway 0 个`，并在 VM 外侧用 `ps` 独立核过（不给下一轮埋跨轮污染）。
- **e3：把原本因无 sudo 而显式 SKIP 的 `diagnostic_aggregator` 那格补成七条真断言**（证到的是“我们的诊断项能进标准诊断栈”；当时我把它写成了“告警沿标准诊断栈传播”，那一步跳了，见下面那条 B-52）：图上只有一个 `/analyzers` 节点、`/diagnostics_toplevel_state` 在、同窗两侧都非空、`telemetry_link` 进了聚合、四个通道项 `*_chN` 进了聚合、**同名项 level 逐条一致**（比了 5 条、不一致 0）、预设“聚合不丢 `hardware_id`”命中。接口是先侦查后断言的（`tools/diagnostics/vm_aggregator_probe.sh`）：参数文件顶层键就是 `analyzers`、聚合输出 `/diagnostics_agg`，而条目名形状是 `/DMP/Channels/CONC_ch3`——**不带**厂商 example 里那个空格，因为带不带取决于上游 `status.name` 自己，不是可依赖的约定（详 B-51）。
- **第一轮四格全红，红的是我自己的探针**（复盘录 B-50）：rclpy 把消息里的 uint8 字段（`DiagnosticStatus.level`）反序列化成**单字节 bytes**，`int(b'\x00')` 抛 ValueError，异常从订阅回调里冒出去打断 `spin_once`，csv 一行不落 → 两侧都读到 0 项。当场能判定“不是聚合栈没工作”只因为**阳性对照并列在两侧**：原始侧也是 0，而 [0] 已确认桥处于 active。那一格如果只断言聚合侧，一句关于生态工具的错结论就会被写进文档——**阳性对照不只防假绿，它也是把一次红归对类的尺子**。
- **全绿不等于证到了**（复盘录 B-52）：那七条里的 `MISMATCH=0` 是**0 对 0 的一致**——那一窗所有 level 都是 OK，所以“告警沿标准诊断栈传播”这句话仍然没有证据，只是被 `PASS=19` 的绿光掩护住了。补了 **[6b] 三格**：读现值与阈值 → 用桥自己的 `/dmp/bed01/set_rule` 把区间整段抬到现值之外（`levelFor` 越界即 Error）→ 断言**原始侧非 OK**（本格自己的阳性对照）、**聚合侧同值**、**阈值恢复原值**。实测 `INJ_RAW_LVL=2 INJ_AGG_LVL=2 INJ_ACCEPT=1 RESTORE=1`，整轮 **`PASS=22 FAIL=0 SKIP=0`**（红→绿→更绿三轮读数全部入库）。写 [6b] 时被自己的 diff 抓住一条**恒真的筛选条件**（字符串拼接把 needle 加进了 haystack）——它在下一格会给“找不到候选”的假象，而不是给假绿。
- 判据从“这一格像不像通过”改成：**“如果那句话是假的，这一格会给出什么读数？”**——答不出这个问题，就不要把这句话写进文档。

1. [X] **网关链路的 Linux 腿实跑（S1，`docs/网关与服务链路优化方案.md` 的起手工序）**：零新代码在**原生 Linux**（Ubuntu 22.04 / gcc 11.4，二进制现场构建自当时 HEAD）跑 `gateway_service` + `device_simulator`，判据 **8/8 PASS**：`STATS ok=144 crc_err=0 dropped=0`；**阳性对照**注入 18 个坏帧→`crc_err=18`（1:1，证明计数器不是死的）；RAW 4 s 透传 1008 B；两订阅者同场且 `STATS` 往返 0.6 ms。方案当年点名的雷（`select` 的 `nfds` 在 POSIX 必须 `max_fd+1`）**实测通过**。

   - 这一轮真正的产出在判据之外：**Linux 腿下游掉线会以 SIGPIPE 终结整个网关进程**，且是阈值型——N≥6 路同时结束时 12/12 格全死、N≤4 路 0/12，而 Windows 用**同一支脚本**两态都 0/12（复盘录 **B-55**）。根因是 `sendAll` 的 `::send(…, 0)` 不带 `MSG_NOSIGNAL`，而进程从未处理该信号；仓库另一条腿（ROS2 桥的客户端）早就带了这个 flag，漏的是**服务端这一条**。修复 `0e06c10`，并把**修复前的二进制留档**与修复后配对跑同一支脚本——否则“0 次死亡”与“探针失效”长得一模一样。
   - 由此风险表补一项并当场消减（IEC §8 **R-010**）；但同一条广播路径上的**慢客户端阻塞（R-009）仍未消减**，那是 T1.1，两者不得混谈。SR-016 也按实况拆成两半：**Linux TCP 上游腿已实跑 ✅**，termios 串口上游（`--serial` 在 POSIX 分支直接 `return 2`）与 epoll **未实施**。
   - 过程中两次自伤都在取证手段上，且症状与被测缺陷同形：`pkill -f` 的字面模式匹配到执行探针的 shell 自身（输出 0 字节文件，读起来像“没有死亡”）、订阅线程里一个未捕获异常静默打死所有测量（报出“0 次死亡”的**假绿**）。它们直接导致了本轮新增的两条前置判据：**阴性结论必须有能报阳的对照**、**跑不满窗口的格子计为无效格**。

## 当前进度（本次已实现并本地验证 ✅）

- ✅ 帧协议 + CRC16 + 粘包/拆包/噪声重同步 解帧器（`include/dmp/frame_protocol.h`）
- ✅ 无锁 SPSC 环形缓冲（`include/dmp/ring_buffer.h`）
- ✅ 采集流水线 + 阈值告警（`include/dmp/acquisition.h`）
- ✅ **纯 C++ 控制台监控端** `monitor_console`（不依赖 Qt，端到端可跑）
- ✅ **单元/集成测试** `tests/test_protocol.cpp`：**27 项断言全绿**（含 10 万元素 SPSC 多线程不丢不重）
- ✅ **端到端冒烟**：`device_simulator` ↔ `monitor_console` 经 TCP 实测 ok 递增、crcErr=0、drop=0
- ✅ **数据持久化**：`include/dmp/storage.h`（CsvSink 落盘 / loadCsv 回放 / TeeSink 扇出）+ `monitor_console --csv <file>` 导出 + `tests/test_storage.cpp` **36 断言全绿**（含 帧→解码→采集→CSV→回放 确定性集成；数字以运行时打印为准——源码 CHECK 行数不等于执行次数）
- ✅ **真实 MCU 数据源（固件侧）**：`firmware/dmp_frame_core.{h,c}` 纯 C 帧内核（无 HAL 依赖）+ `dmp_frame.{h,c}` USART1 发送 glue + 合成采样 `dmp_task()`；`firmware/README.md` 给出并入 Keil 工程的 4 步
- ✅ **上位机裸串口端**：`src/monitor_serial.cpp`（CreateFile 打开 COMx→读原始字节→`dmp::Acquisition` 解码→告警/CSV），Windows 编译通过 + 错误路径已验
- ✅ **协议一致性（关键正确性）**：`tests/test_frame_parity.cpp` **41 断言全绿**——固件 C 内核与上位机 C++ `encodeFrame/FrameDecoder` **逐字节相等** + 交叉解帧 + 金标准帧（CRC/浮点小端/seq 小端）
- ✅ **Qt6 上位机做实**：`src/qt_monitor.cpp` 升级为 滚动曲线 + 危急值横幅 + 状态计数 + SQLite 批量落库 + **TCP/串口双数据源**可选；用 **Qt 自带 cmake+Ninja+MinGW13.1** 对真实 Qt6（Core/Gui/Widgets/Charts/Sql/Network/SerialPort）**编译+链接通过**（`build=0`）。内置 `--verify`（同步自检）/`--selftest <ms>`/`--demo-tcp` 钩子供无头/桌面复验
- ✅ **多线程采集链路**：`include/dmp/pipeline.h` `ProducerThread<Source>` 把阻塞读源（串口 `ReadFile`/socket `recv`）放独立线程，主线程周期 `drain` 渲染；统计量以原子快照发布严守 SPSC 所有权。`monitor_serial`/`monitor_console` 均接入并编译通过；`tests/test_pipeline.cpp` **9 断言全绿**（N=5000 帧、每块 7 字节恶意拆包，不丢不重不乱序）
- ✅ **服务层（后端抽象）**：`include/dmp/service_proto.h` 文本行协议纯函数（`parseCommand`/`format*`）+ `tests/test_service_proto.cpp` **58 断言全绿**（RAW/HISTORY 命令 + RULES/RULE 阈值下发面）；`src/gateway_service.cpp` `select` 单线程 TCP 网关（二进制帧→HELP/STATS/ALARMS/SUBSCRIBE/RAW/HISTORY/RULES/RULE 文本 API，多客户端订阅扇出 + 串口上游 + RAW 逐字节透传），**端到端实跑**；`proto/telemetry.proto` 同语义 gRPC 契约已在 WSL2 编成真服务并串成真机全链路
- ✅ **Qt UI 增强（IEC C 类演练）**：新增 **历史回放页**（`SqliteSink::loadSeries` 读库 + `QSlider` 时间轴滑动窗 + 独立 `histChart_`）与 **告警声光+人工确认**（`QApplication::beep` + `确认告警` 按钮 + 未确认计数）；`qt_monitor.exe` 用 Qt 工具链**重新编译+链接通过**（exit=0）

> ⚠️ 运行时冒烟：本代理会话为**非交互沙箱**，跑 GUI 子系统 Qt 程序会在按需加载 platform/QSQLITE 插件时触发 `STATUS_DLL_INIT_FAILED (0xC0000142)`，属环境限制而非代码缺陷。请在**用户桌面（交互会话）**执行 `build_qt\qt_monitor.exe --verify`（应打印 `... -> PASS`）或双击运行看实时曲线。

> 逐文件的直编命令与端口参数见上文「操作方法 → 附录」。
