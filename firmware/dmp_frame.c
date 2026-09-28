/**
  ******************************************************************************
  * @file    dmp_frame.c
  * @brief   固件侧发送实现 (依赖 usart.c 里的 extern UART_HandleTypeDef huart1)
  ******************************************************************************
  */
#include "dmp_frame.h"
#include "stm32f1xx_hal.h"   /* HAL_UART_Transmit, HAL_GetTick */
#include <math.h>            /* sinf */

extern UART_HandleTypeDef huart1;   /* 见 Core/Src/usart.c */

#define DMP_TX_TIMEOUT   50u          /* ms, 12 字节 @115200 远小于此 */
#define DMP_PERIOD_MS    200u         /* 每通道 5Hz */

static uint16_t g_seq   = 0;
static uint8_t  g_ch    = 0;          /* 轮转通道 0..3 */
static uint32_t g_tick  = 0;          /* 上次上报时刻 */
static uint32_t g_step  = 0;          /* 相位累加 (合成波形用) */

/* 极简线性同余伪随机 (0..1), 避免依赖 stdlib rand */
static float frand(void) {
    static uint32_t s = 0x1234abcdu;
    s = s * 1664525u + 1013904223u;
    return (float)((s >> 8) & 0xFFFFFF) / (float)0xFFFFFF;
}

void dmp_frame_init(void) {
    g_seq = 0; g_ch = 0; g_tick = 0; g_step = 0;
}

void dmp_send_sample(uint8_t channel, dmp_sample_type type, float value) {
    dmp_sample s;
    s.seq     = g_seq++;
    s.channel = channel;
    s.type    = (uint8_t)type;
    s.value   = value;

    uint8_t frame[DMP_FRAME_LEN];
    dmp_build_frame(&s, frame);
    HAL_UART_Transmit(&huart1, frame, DMP_FRAME_LEN, DMP_TX_TIMEOUT);
}

void dmp_task(void) {
    uint32_t now = HAL_GetTick();
    if (now - g_tick < DMP_PERIOD_MS) return;   /* 未到节拍 */
    g_tick = now;
    g_step++;

    /* 四通道轮流上报, 使每通道约每 200ms*4=800ms 更新一次 */
    switch (g_ch) {
        case 0: /* 体温: 36.5 ± 1.0 缓变 */
            dmp_send_sample(0, DMP_TYPE_TEMPERATURE,
                            36.5f + 1.0f * sinf((float)g_step * 0.1f) + 0.1f * frand());
            break;
        case 1: /* 心率: 70 ± 20; 每 15 个心率样本(心率每~0.8s一个, 约 12s)必注入一次越限危急值(125±5)
                 * 专用心率计数 g_hr_n, 避免 g_step/g_seq 与轮转通道相位锁死导致尖峰碰不到本分支 */
            {
                static uint32_t g_hr_n = 0;
                float hr;
                ++g_hr_n;
                if ((g_hr_n % 15u) == 0u) hr = 125.0f + 5.0f * frand();
                else hr = 70.0f + 20.0f * sinf((float)g_step * 0.15f) + 2.0f * frand();
                dmp_send_sample(1, DMP_TYPE_HEART_RATE, hr);
            }
            break;
        case 2: /* 血氧: 97 ± 2 */
            dmp_send_sample(2, DMP_TYPE_SPO2,
                            97.0f + 2.0f * sinf((float)g_step * 0.05f) + 0.3f * frand());
            break;
        case 3: /* 浓度: 0..12 (IVD 模拟) */
            dmp_send_sample(3, DMP_TYPE_CONCENTR,
                            6.0f + 5.0f * sinf((float)g_step * 0.08f) + 0.2f * frand());
            break;
        default: break;
    }
    g_ch = (uint8_t)((g_ch + 1u) & 0x3u);
}
