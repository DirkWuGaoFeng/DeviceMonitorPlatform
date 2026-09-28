/**
  ******************************************************************************
  * @file    dmp_frame_core.c
  * @brief   DeviceMonitorPlatform 帧协议内核实现 (纯 C, 无 HAL 依赖)
  *          MCU 固件与上位机测试共用同一份源码, 保证协议逐字节一致。
  ******************************************************************************
  */
#include "dmp_frame_core.h"
#include <string.h>

uint16_t dmp_crc16_ccitt(const uint8_t* data, size_t len)
{
    uint16_t crc = 0xFFFFu;
    for (size_t i = 0; i < len; ++i) {
        crc ^= (uint16_t)((uint16_t)data[i] << 8);
        for (int b = 0; b < 8; ++b) {
            if (crc & 0x8000u) crc = (uint16_t)((crc << 1) ^ 0x1021u);
            else               crc = (uint16_t)(crc << 1);
        }
    }
    return crc;
}

size_t dmp_build_frame(const dmp_sample* s, uint8_t* out)
{
    out[0] = DMP_FRAME_HEAD0;
    out[1] = DMP_FRAME_HEAD1;
    out[2] = (uint8_t)(s->seq & 0xFFu);
    out[3] = (uint8_t)((s->seq >> 8) & 0xFFu);
    out[4] = s->channel;
    out[5] = s->type;
    /* float -> 4 字节小端: ARM 与 x86 均为小端 IEEE-754, memcpy 直接一致 */
    memcpy(&out[6], &s->value, 4);
    uint16_t crc = dmp_crc16_ccitt(out, 10u);
    out[10] = (uint8_t)(crc & 0xFFu);
    out[11] = (uint8_t)((crc >> 8) & 0xFFu);
    return DMP_FRAME_LEN;
}
