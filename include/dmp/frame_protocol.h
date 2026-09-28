// dmp/frame_protocol.h — 设备上报帧协议 (骨架)
//
// 帧格式 (little-endian, 共 12 字节):
//   +0  +1   : 帧头 0xAA 0x55
//   +2  +3   : uint16 seq        序号 (回绕)
//   +4       : uint8  channel    通道号 (0..N-1)
//   +5       : uint8  type       量纲类型 (见 SampleType)
//   +6..+9   : float  value      测量值
//   +10 +11  : uint16 crc16      对 [0..9] 的 CRC-16/CCITT-FALSE
//
// 该协议同时演示: 帧同步、粘包/拆包处理、CRC 校验。
// 未来接 STM32 时, MCU 端按同样结构打包即可 (注意对齐与字节序)。
#pragma once

#include <cstdint>
#include <cstddef>
#include <cstring>
#include <vector>
#include <optional>

namespace dmp {

constexpr uint8_t  FRAME_HEAD0 = 0xAA;
constexpr uint8_t  FRAME_HEAD1 = 0x55;
constexpr size_t   FRAME_LEN   = 12;

enum SampleType : uint8_t {
    TYPE_TEMPERATURE = 0x01, // ℃
    TYPE_HEART_RATE  = 0x02, // bpm
    TYPE_SPO2        = 0x03, // %
    TYPE_CONCENTR    = 0x04, // 浓度 (IVD 模拟)
    TYPE_PRESSURE    = 0x05, // kPa
};

struct Sample {
    uint16_t   seq     = 0;
    uint8_t    channel = 0;
    SampleType type    = TYPE_TEMPERATURE;
    float      value   = 0.f;
    uint64_t   recv_ts = 0; // 采集侧接收时间戳(ms), 由上层填充
};

// CRC-16/CCITT-FALSE
inline uint16_t crc16_ccitt(const uint8_t* data, size_t len) {
    uint16_t crc = 0xFFFF;
    for (size_t i = 0; i < len; ++i) {
        crc ^= static_cast<uint16_t>(data[i]) << 8;
        for (int b = 0; b < 8; ++b)
            crc = (crc & 0x8000) ? (crc << 1) ^ 0x1021 : (crc << 1);
    }
    return crc;
}

// 编码一帧 (供模拟器/MCU 侧使用)
inline std::vector<uint8_t> encodeFrame(const Sample& s) {
    std::vector<uint8_t> f(FRAME_LEN);
    f[0] = FRAME_HEAD0; f[1] = FRAME_HEAD1;
    f[2] = s.seq & 0xFF;        f[3] = (s.seq >> 8) & 0xFF;
    f[4] = s.channel;
    f[5] = static_cast<uint8_t>(s.type);
    float v = s.value;
    std::memcpy(&f[6], &v, 4);
    uint16_t crc = crc16_ccitt(f.data(), 10);
    f[10] = crc & 0xFF; f[11] = (crc >> 8) & 0xFF;
    return f;
}

// 流式解帧器: 处理粘包/拆包 + CRC 校验, 逐帧吐出
// 用法: 每次收到网络字节就 feed(buf,n), 对每个 onSample 回调处理。
class FrameDecoder {
public:
    template <class OnSample>
    void feed(const uint8_t* data, size_t len, OnSample&& onSample) {
        buf_.insert(buf_.end(), data, data + len);
        size_t pos = 0;
        while (true) {
            // 1) 帧头同步: 丢弃帧头前的噪声字节
            while (pos + 1 < buf_.size() &&
                   !(buf_[pos] == FRAME_HEAD0 && buf_[pos + 1] == FRAME_HEAD1))
                ++pos;
            if (pos + FRAME_LEN > buf_.size()) break;         // 半包, 等待更多
            const uint8_t* fr = buf_.data() + pos;
            // 2) CRC 校验
            uint16_t want = static_cast<uint16_t>(fr[10]) | (fr[11] << 8);
            if (crc16_ccitt(fr, 10) != want) { ++pos; ++stat_.crcErr; continue; } // 错帧, 滑一字节重同步
            // 3) 解析
            Sample s;
            s.seq     = static_cast<uint16_t>(fr[2]) | (fr[3] << 8);
            s.channel = fr[4];
            s.type    = static_cast<SampleType>(fr[5]);
            std::memcpy(&s.value, &fr[6], 4);
            onSample(s);
            ++stat_.ok;
            pos += FRAME_LEN;
        }
        buf_.erase(buf_.begin(), buf_.begin() + pos);          // 保留未消费的半包
    }

    struct Stat { uint64_t ok = 0, crcErr = 0; };
    const Stat& stat() const { return stat_; }

private:
    std::vector<uint8_t> buf_;
    Stat stat_;
};

} // namespace dmp
