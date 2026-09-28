// tests/test_frame_parity.cpp — 证明"固件 C 内核"与"上位机 C++ 协议"逐字节一致
//
// 这是把 TCP 模拟器换成真实 STM32 数据源的关键正确性保证: MCU 用
// firmware/dmp_frame_core.c 编码, 上位机用 include/dmp/frame_protocol.h 解码,
// 只要两端字节布局/CRC 有任何分歧, 这里的断言立刻失败。
//
// 编译时把 firmware/dmp_frame_core.c 一起交给 g++: 该 .c 通过自身头文件里的
// extern "C" 块获得 C 链接, 与本文件的 extern "C" 声明一致。
#include "dmp/frame_protocol.h"
#include "dmp/acquisition.h"
#include "dmp_frame_core.h"   // 纯 C 内核 (extern "C")

#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

static int g_fail = 0;
static int g_run  = 0;
#define CHECK(cond) \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp;

// 用固件内核编码一帧
static std::vector<uint8_t> fwEncode(uint16_t seq, uint8_t ch, uint8_t type, float v) {
    dmp_sample s; s.seq = seq; s.channel = ch; s.type = type; s.value = v;
    uint8_t buf[DMP_FRAME_LEN];
    size_t n = dmp_build_frame(&s, buf);
    return std::vector<uint8_t>(buf, buf + n);
}

static void test_crc_match() {
    SECTION("CRC 内核一致 (固件 dmp_crc16_ccitt == 上位机 crc16_ccitt)");
    const uint8_t vecs[][5] = {
        {0xAA,0x55,0x01,0x00,0x00}, {0x00,0x00,0x00,0x00,0x00}, {0xFF,0xFF,0xFF,0xFF,0xFF},
    };
    for (auto& v : vecs) {
        uint16_t fw = dmp_crc16_ccitt(v, 5);
        uint16_t ho = crc16_ccitt(v, 5);
        CHECK(fw == ho);
    }
    // 空/单字节边界
    CHECK(dmp_crc16_ccitt(reinterpret_cast<const uint8_t*>(""), 0) == crc16_ccitt(reinterpret_cast<const uint8_t*>(""), 0));
}

static void test_encode_parity() {
    SECTION("帧编码逐字节一致 (多组 seq/type/含负与小数 value)");
    struct Case { uint16_t seq; uint8_t ch; SampleType t; float v; };
    Case cases[] = {
        {0,     0, TYPE_TEMPERATURE, 36.5f},
        {1,     1, TYPE_HEART_RATE,  72.0f},
        {65535, 3, TYPE_CONCENTR,    -0.25f},
        {258,   2, TYPE_SPO2,        98.75f},
        {1000,  0, TYPE_PRESSURE,    101.3267f},
        {0,     0, TYPE_TEMPERATURE, 0.0f},
        {4660,  1, TYPE_HEART_RATE,  1e-6f},
    };
    for (auto& c : cases) {
        Sample s; s.seq = c.seq; s.channel = c.ch; s.type = c.t; s.value = c.v;
        auto host = encodeFrame(s);
        auto fw   = fwEncode(c.seq, c.ch, (uint8_t)c.t, c.v);
        CHECK(host.size() == fw.size());
        CHECK(std::memcmp(host.data(), fw.data(), host.size()) == 0);   // 12 字节全等
    }
}

static void test_cross_decode() {
    SECTION("交叉解帧: 固件编码 -> 上位机 FrameDecoder 还原");
    FrameDecoder dec;
    std::vector<Sample> got;
    // 拼多条固件帧 (含跨包喂入的极端情况: 逐字节 feed)
    std::vector<uint8_t> stream;
    for (int i = 0; i < 5; ++i) {
        auto f = fwEncode((uint16_t)(100 + i), (uint8_t)(i % 4),
                          (uint8_t)TYPE_TEMPERATURE, 37.0f + i * 0.5f);
        stream.insert(stream.end(), f.begin(), f.end());
    }
    // 故意一字节一喂, 压测流式同步
    for (uint8_t b : stream) dec.feed(&b, 1, [&](Sample s){ got.push_back(s); });
    CHECK(got.size() == 5);
    if (got.size() == 5) {
        CHECK(dec.stat().ok == 5 && dec.stat().crcErr == 0);
        for (int i = 0; i < 5; ++i) {
            CHECK(got[i].seq == (uint16_t)(100 + i));
            CHECK(got[i].channel == (uint8_t)(i % 4));
            CHECK(std::fabs(got[i].value - (37.0f + i * 0.5f)) < 1e-5f);
        }
    }
}

static void test_golden_bytes() {
    SECTION("金标准帧 (回归锚点: seq=1 ch=1 type=HR value=72.0)");
    auto fw = fwEncode(1, 1, (uint8_t)TYPE_HEART_RATE, 72.0f);
    // 帧头 + seq(小端) + channel + type
    CHECK(fw[0] == 0xAA && fw[1] == 0x55);
    CHECK(fw[2] == 0x01 && fw[3] == 0x00);
    CHECK(fw[4] == 0x01);
    CHECK(fw[5] == 0x02);
    // value=72.0f 的小端字节: 0x00 0x00 0x90 0x42
    CHECK(fw[6] == 0x00 && fw[7] == 0x00 && fw[8] == 0x90 && fw[9] == 0x42);
    // CRC 覆盖 [0..9], 与上位机对同一段的结果一致
    uint16_t crc = (uint16_t)fw[10] | (fw[11] << 8);
    CHECK(crc == crc16_ccitt(fw.data(), 10));
}

int main() {
    test_crc_match();
    test_encode_parity();
    test_cross_decode();
    test_golden_bytes();
    std::printf("\nFRAME-PARITY: run=%d fail=%d -> %s\n", g_run, g_fail,
                g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
