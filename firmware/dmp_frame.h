/**
  ******************************************************************************
  * @file    dmp_frame.h
  * @brief   DeviceMonitorPlatform 固件侧发送 glue: 复用 dmp_frame_core 打包,
  *          经 USART1 (huart1) 发出; 并提供合成传感器采样任务 dmp_task()。
  *
  * 适配工程: 05_sgvalley_monitor (STM32F103, USART1 PA9-TX/PA10-RX @115200)
  ******************************************************************************
  */
#ifndef DMP_FRAME_H
#define DMP_FRAME_H

#include <stdint.h>
#include "dmp_frame_core.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 复位序号发生器 (在 MX_USART1_UART_Init 之后调用一次) */
void dmp_frame_init(void);

/* 组一帧并经 USART1 阻塞发送 (用于接真实传感器时手动上报) */
void dmp_send_sample(uint8_t channel, dmp_sample_type type, float value);

/* 周期任务: 在主循环里反复调用即可。内部按 ~5Hz 轮流产生
   体温/心率/血氧/浓度 四通道合成数据并上报, 每 40 拍给心率注入一次
   假危急值 (+30), 用于上位机告警链路演示。 */
void dmp_task(void);

#ifdef __cplusplus
}
#endif

#endif /* DMP_FRAME_H */
