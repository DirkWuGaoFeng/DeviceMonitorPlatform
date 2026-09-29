# 真机链路：STM32 → 上位机（端到端 bring-up）

> 目标：把招牌项目从"TCP 模拟器"升级到**真实 MCU 数据源**。板子按 `frame_protocol` 同一 12 字节帧经 USART1 发合成生理数据，PC 端裸串口解码/告警/落库，Qt 上位机选"串口"可视化。**全程 Windows 原生，不需要 WSL。**

适配工程（推荐宿主）：`E:\Work\STM32Project\12_usart_printf_hal`（STM32F103，Keil MDK：`MDK-ARM\12_usart_printf_hal.uvprojx`，USART1 PA9/PA10 @115200 8N1）。

> **为什么选 12 而不是 05/09/10**：`dmp_task()` 需要一个**空的、非阻塞的 `while(1)`** 宿主，且 USART1 的发送通路要已验证。12 的主循环是空的、printf 已证明 `huart1` 阻塞发送 OK、且带 CubeMX `USER CODE` 标记（机器可安全插入）。对比：05 的 USART1 被 shell 占用逻辑复杂（首次 `raw=0` 即因此）；09 主循环卡在阻塞 `ReceiveToIdle(HAL_MAX_DELAY)` 回显，`dmp_task()` 轮不到执行；10 是中断回显测试，与发送抢串口。

---

## 阶段一：固件侧接入（板子）

### 1. 一键接入（拷贝 + 打补丁 + 注入工程，全自动）
在本项目根目录（你的控制台，非受限会话）：
```powershell
.\deploy_firmware.ps1 -DryRun   # 先预览将做的改动(可选)
.\deploy_firmware.ps1           # 执行: 默认工程 12_usart_printf_hal
```
它自动完成三件事（**全部幂等，改动前留 `*.dmp.bak`**）：
1. 把 `firmware\` 的 4 个文件拷进 `Core\Src` / `Core\Inc`；
2. 往 `main.c` 的三个 CubeMX `USER CODE` 段精确插行：`#include "dmp_frame.h"`(Includes) / `dmp_frame_init();`(BEGIN 2, 一次性) / `dmp_task();`(BEGIN 3, 循环内)；
3. 往 `uvprojx` 的 `Application/User/Core` 组注入 `dmp_frame_core.c` + `dmp_frame.c` 两个编译单元。

> 编码安全：脚本用 Latin1 字节保真往返，只插纯 ASCII 行，绝不重编码——含 GBK 中文注释的 `main.c` 也不会被破坏（已在演练中逐字节校验：delta=62B、`.bak` 与原文件相等）。
> 换宿主工程：`.\deploy_firmware.ps1 -Project 'E:\Work\STM32Project\09_usart_rolling_hal'`。

### 2. 编译 + 刷录
- Keil **F7** 编译 0 error → **Download(Alt+F7/工具栏)** 经 ST-Link/J-Link 烧录。
- 板子 USART1 的 TX(PA9) 经 **CH340/CP210x USB 转串口** 接 PC；插上后设备管理器出现 `COMx`（本机当前是 **COM4**）。

---

## 阶段二：Windows 上位机验收

### 路径 A：控制台裸串口（最快验证链路通不通）✅ 已实机验证通过
```powershell
.\serial_check.ps1                 # 自动选第一个 COM 口
.\serial_check.ps1 -Port COM4 -Csv history.csv   # 指定口 + 导出历史
```
实测输出（12 工程刷录后）：`raw` 每 200ms +12（恰一帧），TEMP 37.3 / HR 86.1 / SPO2 98.0 / CONC 9.0 实时刷新，均落在模拟器值域——真机解码链路打通。
期望终端滚动刷新：
```
TEMP= 36.8 ██████  HR=  74 ███  SPO2=  97 █████  CONC= 5.2 ██ | ok=123 crcErr=0 drop=0
```
- **`ok` 持续递增、`crcErr=0`** → 帧协议跨端字节级一致（固件 `dmp_build_frame` 与上位机 `FrameDecoder` 同源，已由 `tests/test_frame_parity.cpp` 41 断言逐字节证明）。
- 心率每 40 拍注入 +30 假危急值 → 触发 `ALARM`（>110）。

### 路径 B：Qt 图形上位机（一键连真机）
```powershell
.\run_qt_serial.ps1                # 自动停掉占用 COM4 的 monitor_serial → 起 GUI → 启动即连串口
.\run_qt_serial.ps1 -Port COM3     # 换口
```
等价手动步骤：直接开 `build_qt\qt_monitor.exe --demo-serial COM4`，或在界面里源下拉选 **"串口 (STM32)"** → 选 COM4 → 波特 115200 → **连接**。可看实时曲线、告警横幅+确认消声、历史回放页（SQLite 落库）。
无头回归验证（不需桌面）：`qt_monitor.exe --verify` → `VERIFY ok=50 crcErr=0 drained=50 db_rows=N(+50) chart_pts=50 N=50 db=open -> PASS`（此验证顺带修掉了一个 `status_` 未创建即使用的构造期空指针崩溃）。
> 注：落库一项现在校的是**增量**。旧写法直接拿 `COUNT(*)` 与 50 比，而 `device_history.db` 是跨次运行的持久文件，
> 于是这个自检只在空库时成立——本地复跑与 CI 都会得到一个看不出原因的 `exit 1`（见 `docs/Bug复盘与判断记录.md` B-02）。

---

## 阶段三：真机 → gRPC 全链路（设备→网关→服务→网络）✅ 已实跑通过

数据流：`STM32(USART1) → gateway_service --serial COM4 (TCP :9100, 文本行 API + RAW 字节透传) → WSL2 dmp_grpc_server (连上游收原始帧再解码, :50051) → grpc_client_probe`

```powershell
# Windows 侧(窗口 1): 起串口网关 (自动释放 COM4)
.\run_gateway_serial.ps1
```
```bash
# WSL 侧(窗口 2): 起 gRPC 服务连 Windows 网关 + probe 验收
bash e2e_realdevice.sh 172.22.176.1     # IP=WSL 默认网关(即 Windows 宿主), 脚本会自动探测 Windows 侧 IP
```
实测证据（真机帧流）：`[STATS] ok=34 crc_err=0 dropped=0` + Subscribe 收到 10 条带真机 seq 的样本（TEMP 37.36 / HR 51.1 / SPO2 96.1 / CONC 10.4）。

关键设计：网关新增 `RAW` 命令（已进 `service_proto.h` 协议层+单测）——订阅者收到与上游逐字节相同的原始帧流，gRPC 服务把串口字节喂给自己的 `FrameDecoder`，**同一帧协议贯穿 UART/TCP/gRPC 三段传输**。WSL 连 Windows 用默认网关 IP（`ip route | awk '/default/{print $3}'`），网关 bind INADDR_ANY 无需改防火墙。

---

## 阶段四：换成真实传感器（可选）
`dmp_task()` 现产生合成数据。接真实外设时，**保持 `dmp_send_sample(channel, type, value)` 不变**，把采样来源从 `sinf(...)` 换成 ADC / DHT11 / 心率模块读数即可——**帧协议与上位机侧完全不用改**（这正是把通信"设备即对象"经验平移到仪器的卖点）。

---

## 故障排查

| 现象 | 大概率原因 | 处理 |
|---|---|---|
| `serial_check` 报"无可用串口" | USB 转串口未插/驱动缺 | 装 CH340 或 CP210x 驱动；换 USB 口 |
| 打开 COM 口失败/占用 | 端口被别的程序占用 | 关掉串口助手/上一个 monitor_serial；确认没被 Qt 占 |
| 有数据但 `crcErr` 猛增、`ok` 不涨 | **波特率不匹配**(板子非 115200) 或 时钟偏差 | 核对 `usart.c` BaudRate=115200；`-Baud` 试 9600 等 |
| 完全无数据 | `dmp_task()` 未进主循环 / TX 线没接 / 未刷录成功 | 确认改点(c)生效；量 PA9 有无波形；重刷 |
| 全是乱码 | 电平/波特率错 | 检查是否误接了调试串口(TTL 电平)而非 USART1 |
| Qt 起不来/缺 DLL | 未在交互桌面 或 PATH 无 Qt bin | 用 `run_qt_serial.ps1`(它会注入 Qt bin 到 PATH)；别在本代理沙箱跑 GUI |

---

## 与模拟器/TCP 模式的关系
`device_simulator(TCP:9000)` 和 `STM32(串口 COMx)` 是**同一解码/告警内核的两个数据源**，上位机侧零差异——这也是"传输层可替换"的实证：UART ↔ TCP ↔ gRPC 三选一，业务层不动。

> 一句话验收标准：**板子一刷，`serial_check.ps1` 里 `ok` 往上跳、`crcErr` 恒为 0、心率偶发 ALARM**，真机链路即打通。
