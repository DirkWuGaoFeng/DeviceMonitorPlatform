/**
  ******************************************************************************
  * @file    dmp_frame_core.h
  * @brief   DeviceMonitorPlatform 帧协议 —— 纯 C 内核 (无 HAL 依赖)
  *
  * 与上位机 include/dmp/frame_protocol.h 严格同构的 12 字节帧:
  *   +0 +1   : 帧头 0xAA 0x55
  *   +2 +3   : uint16 seq        小端
  *   +4      : uint8  channel
  *   +5      : uint8  type        (见 dmp_sample_type)
  *   +6..+9  : float  value       小端 IEEE-754
  *   +10+11  : uint16 crc16       CRC-16/CCITT-FALSE, 覆盖 [0..9], 小端
  *
  * 该内核刻意不依赖任何 STM32/HAL 头, 因此同一份 .c 既能进 MCU 固件,
  * 也能在 PC 上编译进上位机测试(tests/test_frame_parity.cpp), 逐字节证明
  * "固件侧编码 == 上位机侧解码" 的协议一致性。
  ******************************************************************************
  */
#ifndef DMP_FRAME_CORE_H
#define DMP_FRAME_CORE_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define DMP_FRAME_HEAD0   0xAAu
#define DMP_FRAME_HEAD1   0x55u
#define DMP_FRAME_LEN     12u

/* 量纲类型: 与上位机 dmp::SampleType 取值一一对应 */
typedef enum {
    DMP_TYPE_TEMPERATURE = 0x01, /* ℃      */
    DMP_TYPE_HEART_RATE  = 0x02, /* bpm    */
    DMP_TYPE_SPO2        = 0x03, /* %      */
    DMP_TYPE_CONCENTR    = 0x04, /* 浓度   */
    DMP_TYPE_PRESSURE    = 0x05  /* kPa    */
} dmp_sample_type;

typedef struct {
    uint16_t seq;
    uint8_t  channel;
    uint8_t  type;   /* dmp_sample_type */
    float    value;
} dmp_sample;

/* CRC-16/CCITT-FALSE: init 0xFFFF, poly 0x1021, 无输出异或 */
uint16_t dmp_crc16_ccitt(const uint8_t* data, size_t len);

/* 把一帧编码进 out (out 至少 DMP_FRAME_LEN 字节), 返回写入字节数 */
size_t dmp_build_frame(const dmp_sample* s, uint8_t* out);

#ifdef __cplusplus
}
#endif

#endif /* DMP_FRAME_CORE_H */
