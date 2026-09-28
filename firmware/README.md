# 固件侧接入指南（STM32F103）

> **推荐宿主工程：`12_usart_printf_hal`**（主循环为空、USART1 阻塞发送已验证、带 CubeMX `USER CODE` 标记）。
> 首选**一键自动接入**（代替下面手动 4 步）：在 `DeviceMonitorPlatform` 根目录跑 `.\deploy_firmware.ps1`（详见 `../docs/真机链路-STM32到上位机.md`）。

把 `DeviceMonitorPlatform` 的招牌项目从"TCP 模拟器"升级为"**真实 MCU 数据源**"。
本目录的帧内核与上位机 `include/dmp/frame_protocol.h` **同源**（同一 CRC、同一 12 字节布局），
`tests/test_frame_parity.cpp` 会逐字节证明两端一致。

## 文件职责

| 文件 | 依赖 | 作用 |
|---|---|---|
| `dmp_frame_core.h/.c` | 纯 C，**无任何 HAL/STM32 头** | 帧编码 `dmp_build_frame()` + `dmp_crc16_ccitt()`；PC 与 MCU 共用 |
| `dmp_frame.h/.c` | 依赖 `stm32f1xx_hal.h` 与 `extern huart1` | 经 USART1 发送 + 合成四通道采样任务 `dmp_task()` |

## 并入 Keil 工程（手动 4 步；`deploy_firmware.ps1` 已能自动完成 1–3 步）

1. **拷贝文件**：把本目录 4 个文件复制到 `05_sgvalley_monitor/Core/Src`（头放 `Core/Inc`）。
   在 Keil 工程树 `Application/User/Core` 分组里 Add Existing Files → 选 `dmp_frame_core.c`、`dmp_frame.c`。
2. **勾选 MicroLIB / 数学库**：`dmp_frame.c` 用到 `sinf`，Keil ARMCC 默认带 libm，无需额外设置；
   若用 GCC/arm-none-eabi 链接报缺 `sinf`，在链接参数加 `-lm`。
3. **初始化**：`main.c` 顶部 `#include "dmp_frame.h"`；在 `MX_USART1_UART_Init();` 之后加一行 `dmp_frame_init();`。
4. **喂任务**：在 `while(1)` 主循环里（`shell_poll()` 附近）加一行 `dmp_task();`。它自带节拍（内部按 `HAL_GetTick()` 限速），
   空转开销极小。

改完后主循环形如：
```c
MX_USART1_UART_Init();
dmp_frame_init();
...
while (1) {
    shell_poll();
    beep_poll();
    dmp_task();          // ← 新增: 周期性把合成样本按帧协议从 USART1 发出
    ...
}
```

## 接线与上位机

- 板子 USART1：PA9=TX、PA10=RX，115200 8N1，经 CH340/CP210x USB 转串口接 PC。
- PC 端在设备管理器看到 `COMx`（本机会分配，如 COM4）。
- 运行上位机串口版：
  ```
  monitor_serial.exe COM4            # 裸串口读帧→解码→告警→终端渲染
  monitor_serial.exe COM4 --csv out.csv   # 同时导出历史
  ```
  看到 `ok` 递增、四通道实时值滚动、心率偶发 `ALARM`，即打通"真实 MCU → 上位机"全链路。

## 换成真实传感器

`dmp_task()` 目前产生合成数据（体温/心率/血氧/浓度）。接入真实外设时，
保留 `dmp_send_sample(channel, type, value)` 不变，把采样来源从 `sinf(...)` 换成
ADC/DHT11/心率模块读数即可——帧协议与上位机侧完全不用改。
