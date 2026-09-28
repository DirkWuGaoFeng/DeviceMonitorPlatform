# DeviceMonitorPlatform — 多仪器数据采集与监控上位机平台（骨架）

<!-- CI 状态徽章: 指向 DirkWuGaoFeng/DeviceMonitorPlatform, Actions 两条门禁腿都绿后亮起 -->
[![CI](https://github.com/DirkWuGaoFeng/DeviceMonitorPlatform/actions/workflows/ci.yml/badge.svg)](https://github.com/DirkWuGaoFeng/DeviceMonitorPlatform/actions/workflows/ci.yml)

> 招牌收敛项目。一条数据链把 **医疗上位机 / 设备平台 / AI 运维** 三个方向焊在一起：
>
> `[下位机/仪器] ──串口/TCP──> [采集与解码] ──环形缓冲──> [存储+告警] ──> [Qt 上位机实时曲线]`
> 上层再挂 **LLM 运维 Agent**（复用 FiberMaintain/Agent）。

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
    ├── IEC62304-软件生命周期与风险管理.md   # 合规文档 + 可追溯矩阵(SR-001~013) + 风险分析(R-001~008)
    ├── WSL-gRPC-落地指南.md      # 在 WSL2 把 proto 契约编成真 gRPC 服务的分步指南
    ├── grpc-verify-log.md        # gRPC 服务层一次完整构建+端到端自检通过的真实输出(可复现证据)
    ├── 真机链路-STM32到上位机.md  # 固件并入 Keil + 刷录 + 串口验收 + 真机→gRPC 全链路 端到端 bring-up
    ├── 运维Agent-诊断指南.md      # 自然语言→告警归因：Agent 工具化网关命令 + 离线/LLM 双模式
    └── 经典Bug素材录.md         # 25 条真实 bug 的"现象→误判→根因→可讲点"，面试可直接讲
```

## 构建（核心，不依赖 Qt）

```bash
cmake -S . -B build -DDMP_BUILD_QT=OFF
cmake --build build -j
./build/device_simulator            # 终端A: 启动模拟设备 :9000
```

## 构建（含 Qt6 上位机）

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

## 落地路线（对应 8 周计划 W3-5）

1. [x] 帧协议 + CRC + 粘包拆包
2. [x] 用 **STM32 板** 替换 `device_simulator`：`firmware/` 按同一帧协议经 USART1 发帧 + 上位机裸串口接入。**已实机验收通过**（12_usart_printf_hal 刷录后 raw/ok 同步增长，四路体征实时解码，工具链：`deploy_firmware.ps1` 一键接入 + `serial_check.ps1` 串口验收 + `docs/真机链路-STM32到上位机.md`）；Qt GUI 一键连真机：`run_qt_serial.ps1`（启动即 `--demo-serial COM4`）；无头回归 `qt_monitor --verify` PASS(50/50 解码=落库=曲线对齐)
3. [x] 环形缓冲接入真实多线程采集线程（`pipeline.h` ProducerThread：读源独立线程 + 原子统计快照；串口/控制台均接入）
4. [x] 历史落库 + 回放 + CSV 导出（`storage.h` CSV；Qt 端 SQLite + **历史回放页/时间轴滑动窗**）
5. [x] 危急值阈值告警（控制台横幅 + Qt 告警条 + **声音 `QApplication::beep` + 人工确认按钮**，演练 IEC 62304 C 类）
6. [x] 把采集/告警后端抽象成服务（两种同语义实现：Windows 侧 `gateway_service.cpp` TCP 文本网关；**WSL2 侧 `service/grpc_server.cpp` 真 gRPC**（实现 `proto/telemetry.proto` 的 GetStats/GetAlarms/Subscribe）；共用同一 `Acquisition` 内核，业务层不动只换传输层）。**已串成真机全链路**：STM32→串口网关(RAW 透传)→WSL gRPC→probe，实测 `ok=34 crc_err=0` + 10 条真机样本流式到达（`e2e_realdevice.sh`）
7. [x] 挂 LLM 运维 Agent：“3 号通道为何频繁告警？”（`tools/dmp_agent.py`，纯 stdlib）：网关新增 `HISTORY [ch] [秒]` 逐通道窗口聚合命令供归因；Agent 把 STATS/ALARMS/HISTORY/SUBSCRIBE 封装为**工具**，双模式：离线确定性诊断（无密钥、CI 可跑）+ OpenAI 兼容 function-calling 循环（`--llm`，环境变量指端点）。真机实跑：正确归因“基线均值 74.8 正常、峰值冲 129.6 = 周期性越限尖峰(固件演示注入)，非链路故障”（详见 `docs/运维Agent-诊断指南.md`）
8. [x] 按 `docs/IEC62304-软件生命周期与风险管理.md` 补齐可追溯矩阵与风险分析：安全分级定档（Agent 归 A 类、告警链路 C 类）；风险表 6 项（R-001~006）补全严重度/概率/风险评价/控制措施→验证闭环 + 剩余风险/收益-风险/PMCF 评价；可追溯矩阵扩至 **SR-001~011**（新增 SR-010 真机→gRPC 全链路、SR-011 运维 Agent；SR-005 改真机已实机验收、SR-008 断言 15→25），**171 项核心断言全绿**且需求↔测试双向闭环
9. [x] 挂 **CI 质量门禁**（`.github/workflows/ci.yml`）：Linux gcc Release/Debug 作红绿灯门禁（已本地同构实测），clang 为咨询腿（`continue-on-error`），Windows 走 MSYS2 MinGW；**上 CI 过程中顺手修掉 3 个真可移植性 bug**（详 `docs/IEC62304...md` §6 回归策略）
10. [x] **接入 ROS2 Humble**（`ros2/`）：`dmp_msgs` 定契约，`dmp_ros2_bridge` 把网关的帧流与阈值面接成 `/dmp/frames`（协议保真）+ `/diagnostics`（标准 `diagnostic_msgs` 视图）+ `/dmp/rules`（锁存）+ 三个服务；VM 内实跑端到端（`ok=396 crc_err=0`，`set_rule` 写入后**从网关侧独立回读核对**，非法区间被拒）。产出 `docs/adr/ADR-001/002` + `ros2/README.md` 的 **QoS 实测矩阵**（推翻了设计时“pub reliable + sub best_effort 会静默”的错误假设，真正错的方向是 pub best_effort + sub reliable）；过程中修掉一个最贵的跨平台 bug：`device_simulator` 的 `select(0,...)` 在 POSIX 下等于不检查任何描述符，accept 永不发生（症状：“连接正常、零字节”）
11. [x] **跨机真板接入 ROS2**（`tools/vm_realdevice_e2e.sh`）：STM32→COM4→Windows 网关→（TCP 跨机）→VM 内桥→ROS2。实测 `/dmp/frames` 33 条真帧、`/diagnostics decoded_ok=89 crc_err=0`、`selftest healthy=True ok=3034`；从 ROS 侧 `set_rule` 后**网关侧独立回读**得 `HR low=40.000 high=105.000`，倒置区间 `accepted=False`；脚本末尾自动还原现场
12. [x] **查清 CI 那条 `exit code 1`（1）修掉一个真 bug**：本地直接跑 `qt_monitor --verify` 即复现 `db_rows=358 -> FAIL`。根因是自检拿 `SELECT COUNT(*)` **绝对值**与 N 比，而 `device_history.db` 是跨次运行的持久文件 —— **这个自检从设计上只在空库时成立**。改为增量计数后本地连跑 3 次 PASS（`+50` 恒定）
13. [x] **查清 CI 那条 `exit code 1`（2）结论反转 → 注解锁定根因 → 已修复**：逐步核实后，失败**步骤**是 `Build qt_monitor`（CMake configure 阶段），`Link check` 与 `Headless verify` 都是 **skipped** —— 所以那个 exit 1 **与 `--verify` 无关**（上面那个是独立真 bug，只是排在挂点之后）。Actions 日志正文需登录（REST 端点返 403），于是把该腿改成“失败时用 `::error::` 把 `CMake Error` / `error:` / `undefined reference` grep 成注解”——注解在 Summary 页**匿名可读**，不需要任何人交凭据。**该设计第一次跑就兑现**：Run #4 拿到报错行 `CMake Error at CMakeLists.txt:44 (find_package)`；根因是 `install-qt-action` 的 `modules:` 只点了 `qtcharts`，而 `find_package` 还点名了 `SerialPort`（Qt 模块不随 qtbase 打包；本地是安装向导“全选”所以永远复现不了）。补上 `qtserialport` 后 **Run #5：Qt6 job 8 步全 success、Summary 页 0 errors**（详 `docs/经典Bug素材录.md` B-24/B-25）
14. [x] **桥节点生命周期化 + 组件容器**（`LifecycleNode`、`rclcpp_components`、`launch/dmp_bridge_composed.launch.py`）：把“采集开没开”从“看话题有没有数据”变成**可查询可断言的状态机**——`tools/vm_lifecycle_compose.sh` 三层负例 **PASS=10 FAIL=0**（unconfigured 无话题无服务 / inactive 话题在但 6s 零批且 `set_rule` 被拒并回显 `state=inactive` / active 6s 30 批 / cleanup 后话题从图上消失）。**但我为架构决策设计的 A/B 实验把决策本身推翻了**：控制面饱和（201 次调用/平均往返 650ms）下同容器另一台设备帧最大间隔基本不变（典型 212→250ms），`mt` 与 `st` 无可测差异；为不让“两轮”撑起“停顿全 0”这种全称断言，**同一脚本不改一行重复到 6 轮**：两臂“打满−空载”差值符号在轮次间翻转（中位数 −8.2 / −2.1 ms），12 个打满窗口共 1 次 >400ms 停顿且落在**应当免疫的 mt 臂** → 两个回调组从“性能必需”降级为“便宜的保险”，真实控制点改写为“任何单个回调必须短”（详 `docs/adr/ADR-003`、素材录 §7 B-26~B-36）。本轮另修掉四个真构建/验证错：`on_activate` 未链回基类导致 publisher 永不激活、OBJECT 库缺 PIC、bundle 缺前置提交造成的“旧代码编译通过”假绿、**写死在验收脚本里的结论行（数据出现反例时它照样打印）**

## 当前进度（本次已实现并本地验证 ✅）

- ✅ 帧协议 + CRC16 + 粘包/拆包/噪声重同步 解帧器（`include/dmp/frame_protocol.h`）
- ✅ 无锁 SPSC 环形缓冲（`include/dmp/ring_buffer.h`）
- ✅ 采集流水线 + 阈值告警（`include/dmp/acquisition.h`）
- ✅ **纯 C++ 控制台监控端** `monitor_console`（不依赖 Qt，端到端可跑）
- ✅ **单元/集成测试** `tests/test_protocol.cpp`：**27 项断言全绿**（含 10 万元素 SPSC 多线程不丢不重）
- ✅ **端到端冒烟**：`device_simulator` ↔ `monitor_console` 经 TCP 实测 ok 递增、crcErr=0、drop=0
- ✅ **数据持久化**：`include/dmp/storage.h`（CsvSink 落盘 / loadCsv 回放 / TeeSink 扇出）+ `monitor_console --csv <file>` 导出 + `tests/test_storage.cpp` **35 断言全绿**（含 帧→解码→采集→CSV→回放 确定性集成）
- ✅ **真实 MCU 数据源（固件侧）**：`firmware/dmp_frame_core.{h,c}` 纯 C 帧内核（无 HAL 依赖）+ `dmp_frame.{h,c}` USART1 发送 glue + 合成采样 `dmp_task()`；`firmware/README.md` 给出并入 Keil 工程的 4 步
- ✅ **上位机裸串口端**：`src/monitor_serial.cpp`（CreateFile 打开 COMx→读原始字节→`dmp::Acquisition` 解码→告警/CSV），Windows 编译通过 + 错误路径已验
- ✅ **协议一致性（关键正确性）**：`tests/test_frame_parity.cpp` **41 断言全绿**——固件 C 内核与上位机 C++ `encodeFrame/FrameDecoder` **逐字节相等** + 交叉解帧 + 金标准帧（CRC/浮点小端/seq 小端）
- ✅ **Qt6 上位机做实**：`src/qt_monitor.cpp` 升级为 滚动曲线 + 危急值横幅 + 状态计数 + SQLite 批量落库 + **TCP/串口双数据源**可选；用 **Qt 自带 cmake+Ninja+MinGW13.1** 对真实 Qt6（Core/Gui/Widgets/Charts/Sql/Network/SerialPort）**编译+链接通过**（`build=0`）。内置 `--verify`（同步自检）/`--selftest <ms>`/`--demo-tcp` 钩子供无头/桌面复验
- ✅ **多线程采集链路**：`include/dmp/pipeline.h` `ProducerThread<Source>` 把阻塞读源（串口 `ReadFile`/socket `recv`）放独立线程，主线程周期 `drain` 渲染；统计量以原子快照发布严守 SPSC 所有权。`monitor_serial`/`monitor_console` 均接入并编译通过；`tests/test_pipeline.cpp` **9 断言全绿**（N=5000 帧、每块 7 字节恶意拆包，不丢不重不乱序）
- ✅ **服务层（后端抽象）**：`include/dmp/service_proto.h` 文本行协议纯函数（`parseCommand`/`format*`）+ `tests/test_service_proto.cpp` **58 断言全绿**（RAW/HISTORY 命令 + RULES/RULE 阈值下发面）；`src/gateway_service.cpp` `select` 单线程 TCP 网关（二进制帧→HELP/STATS/ALARMS/SUBSCRIBE/RAW/HISTORY/RULES/RULE 文本 API，多客户端订阅扇出 + 串口上游 + RAW 逐字节透传），**端到端实跑**；`proto/telemetry.proto` 同语义 gRPC 契约已在 WSL2 编成真服务并串成真机全链路
- ✅ **Qt UI 增强（IEC C 类演练）**：新增 **历史回放页**（`SqliteSink::loadSeries` 读库 + `QSlider` 时间轴滑动窗 + 独立 `histChart_`）与 **告警声光+人工确认**（`QApplication::beep` + `确认告警` 按钮 + 未确认计数）；`qt_monitor.exe` 用 Qt 工具链**重新编译+链接通过**（exit=0）

> ⚠️ 运行时冒烟：本代理会话为**非交互沙箱**，跑 GUI 子系统 Qt 程序会在按需加载 platform/QSQLITE 插件时触发 `STATUS_DLL_INIT_FAILED (0xC0000142)`，属环境限制而非代码缺陷。请在**用户桌面（交互会话）**执行 `build_qt\qt_monitor.exe --verify`（应打印 `... -> PASS`）或双击运行看实时曲线。

> 本地直编验证（MinGW g++，无需 Qt）：
> ```bash
> g++ -std=c++17 -pthread -Iinclude tests/test_protocol.cpp -o test_protocol && ./test_protocol
> g++ -std=c++17 -Iinclude tests/test_storage.cpp -o test_storage && ./test_storage
> g++ -std=c++17 -Iinclude src/device_simulator.cpp -lws2_32 -o device_simulator
> g++ -std=c++17 -Iinclude src/monitor_console.cpp -lws2_32 -o monitor_console
> g++ -std=c++17 -Iinclude src/monitor_serial.cpp -o monitor_serial   # 裸串口接真实 STM32
> g++ -std=c++17 -Iinclude -Ifirmware tests/test_frame_parity.cpp firmware/dmp_frame_core.c -o test_frame_parity && ./test_frame_parity
> # 终端A ./device_simulator 9000 ; 终端B ./monitor_console 127.0.0.1 9000 out.csv   (第3个参数为导出 CSV)
> ```
> 亦可 `cmake -S . -B build && cmake --build build`（Qt 上位机加 `-DDMP_BUILD_QT=ON`）。

## 面试可讲点

- 帧同步与粘包拆包、CRC 校验、超时重传 → 设备通信链路健壮性
- 无锁环形缓冲 + 采集/UI 线程分离 → 高并发数据链路不丢包
- 阈值判定与危急值告警 → 安全关键（对应医疗器械软件分级）
- 全流程按 IEC 62304 V 模型 + ISO 14971 风险管理组织 → 法规护城河
- ROS2 桥的双层接口（保真 topic + 标准诊断视图）与写路径安全边界 → “能写什么”比“能写多方便”先想清楚
- QoS 不匹配是 ROS2 里唯一“发现得到、不报错、就是没数据”的故障类 → 用四格实测矩阵拿结论，而不是背文档
- `select()` nfds 语义差异导致“连接正常、零字节” → 跨平台移植时现象离根因有多远
- `--verify` 拿 `COUNT(*)` 绝对值做断言 → **自检脚本自己不可重跑**比没自检更坑
- CI 的 exit 1 并不等于“自检判 FAIL”：先用公开 API 拿到逐步结论，发现失败在**构建阶段**，后两步 skipped → **先定住“挂在哪一步”，再谈根因**；日志不可读就把根因 grep 成匿名可读的注解
- 本地“全选”装了全部 Qt 模块，CI 的 `modules:` 白名单少一个 → **本地永远复现不了这个差异**；依赖清单存在两个不同构的声明面本身就是 bug
- `timeout` 默认 SIGTERM 不 flush → 被杀进程丢块缓冲，**“0 条数据”可能是取证手段自己造的假**
