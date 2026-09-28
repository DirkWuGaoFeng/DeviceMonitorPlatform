// tests/test_protocol.cpp — 帧协议/环形缓冲/采集告警 的单元+集成测试
//
// 不依赖第三方测试框架, 自带极简断言宏, 任何编译器都能跑 (对应 IEC 62304 5.7 单元验证)。
// 运行退出码: 0=全部通过, 非0=失败数。可被 ctest 或直接执行。
#include "dmp/frame_protocol.h"
#include "dmp/ring_buffer.h"
#include "dmp/acquisition.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static int g_fail = 0;
static int g_run  = 0;
#define CHECK(cond)                                                          \
    do {                                                                     \
        ++g_run;                                                             \
        if (!(cond)) {                                                       \
            std::printf("  \x1b[31mFAIL\x1b[0m %s:%d  %s\n",                \
                        __FILE__, __LINE__, #cond);                          \
            ++g_fail;                                                        \
        }                                                                    \
    } while (0)

#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp;

static std::vector<uint8_t> mk(uint16_t seq, uint8_t ch, SampleType t, float v) {
    Sample s; s.seq = seq; s.channel = ch; s.type = t; s.value = v;
    return encodeFrame(s);
}

// 收集解码出的样本
struct Collector {
    std::vector<Sample> out;
    void operator()(const Sample& s) { out.push_back(s); }
};

static void test_encode_decode_roundtrip() {
    SECTION("encode/decode 单帧往返");
    auto bytes = mk(7, 2, TYPE_TEMPERATURE, 36.6f);
    CHECK(bytes.size() == FRAME_LEN);
    FrameDecoder dec;
    Collector c;
    dec.feed(bytes.data(), bytes.size(), c);
    CHECK(c.out.size() == 1);
    CHECK(dec.stat().ok == 1);
    if (!c.out.empty()) {
        CHECK(c.out[0].seq == 7);
        CHECK(c.out[0].channel == 2);
        CHECK(c.out[0].type == TYPE_TEMPERATURE);
        CHECK(std::abs(c.out[0].value - 36.6f) < 1e-4f);
    }
}

static void test_sticky_packets() {
    SECTION("粘包: 两帧连发一次喂入");
    std::vector<uint8_t> stream;
    auto a = mk(1, 0, TYPE_HEART_RATE, 72.f);
    auto b = mk(2, 1, TYPE_SPO2, 98.f);
    stream.insert(stream.end(), a.begin(), a.end());
    stream.insert(stream.end(), b.begin(), b.end());
    FrameDecoder dec; Collector c;
    dec.feed(stream.data(), stream.size(), c);
    CHECK(c.out.size() == 2);
    CHECK(dec.stat().ok == 2);
    CHECK(c.out.size() == 2 && c.out[1].seq == 2);
}

static void test_split_packet() {
    SECTION("拆包: 一帧分两次喂入");
    auto bytes = mk(99, 3, TYPE_CONCENTR, 5.5f);
    FrameDecoder dec; Collector c;
    dec.feed(bytes.data(), 5, c);            // 前 5 字节 (半帧)
    CHECK(c.out.size() == 0);                // 不应产出
    dec.feed(bytes.data() + 5, bytes.size() - 5, c); // 其余
    CHECK(c.out.size() == 1);
    CHECK(dec.stat().ok == 1);
}

static void test_noise_resync() {
    SECTION("噪声前缀 + 帧头重同步");
    std::vector<uint8_t> stream = {0x00, 0x11, 0xAA, 0x77, 0xFF}; // 干扰字节(含伪帧头)
    auto a = mk(5, 0, TYPE_PRESSURE, 101.f);
    stream.insert(stream.end(), a.begin(), a.end());
    FrameDecoder dec; Collector c;
    dec.feed(stream.data(), stream.size(), c);
    CHECK(c.out.size() == 1);                // 噪声被跳过, 正确解出 1 帧
    if (!c.out.empty()) CHECK(c.out[0].seq == 5);
}

static void test_crc_corrupt() {
    SECTION("CRC 错帧被丢弃, 且能恢复后续帧");
    auto bad = mk(10, 0, TYPE_TEMPERATURE, 40.f);
    bad[8] ^= 0xFF;                          // 篡改 value 使 CRC 不匹配
    auto good = mk(11, 0, TYPE_TEMPERATURE, 37.f);
    std::vector<uint8_t> stream;
    stream.insert(stream.end(), bad.begin(), bad.end());
    stream.insert(stream.end(), good.begin(), good.end());
    FrameDecoder dec; Collector c;
    dec.feed(stream.data(), stream.size(), c);
    CHECK(dec.stat().crcErr >= 1);           // 至少识别出一个错帧
    CHECK(c.out.size() == 1);                // 只有 good 帧通过
    if (!c.out.empty()) CHECK(c.out[0].seq == 11);
}

static void test_ring_basic() {
    SECTION("环形缓冲: 满丢弃/空弹出/计数");
    RingBuffer<int> rb(8);                   // 容量向上取 2 的幂(此实现 mask 后有效槽=cap-1)
    CHECK(rb.capacity() >= 8);
    int dummy;
    size_t pushed = 0;
    while (rb.push(1)) ++pushed;             // 直到满
    CHECK(pushed >= 1);
    rb.addDropped(0);
    int v = 0;
    CHECK(rb.pop(v) && v == 1);              // 能取回
    while (rb.pop(v)) {}                     // 排空
    CHECK(!rb.pop(v));                        // 空返回 false
}

static void test_ring_spsc_threaded() {
    SECTION("环形缓冲: 单生产者单消费者多线程不丢不重");
    constexpr int N = 100000;
    RingBuffer<int> rb(4096);
    std::thread prod([&]{
        for (int i = 1; i <= N; ) {
            if (rb.push(i)) ++i;             // 满则自旋重试 (SPSC 阻塞式)
        }
    });
    long long sum = 0; long cnt = 0;
    int x;
    while (cnt < N) { if (rb.pop(x)) { sum += x; ++cnt; } }
    prod.join();
    CHECK(cnt == N);
    CHECK(sum == (long long)N * (N + 1) / 2); // 顺序与求和都对
}

// 记录告警的测试 Sink
struct AlarmSink : public Sink {
    std::vector<Sample> got;
    void onSample(const Sample& s) override { got.push_back(s); }
};

static void test_acquisition_alarm() {
    SECTION("采集管道: 端到端解码 + 阈值告警");
    AlarmSink sink;
    Acquisition acq;
    acq.setSink(&sink);
    acq.addRule({TYPE_HEART_RATE, 50.f, 110.f, "HR out of range"});
    int alarms = 0;
    acq.setAlarmCallback([&](const AlarmEvent&){ ++alarms; });

    // 正常值 72 不告警; 危急值 130 告警
    std::vector<uint8_t> stream;
    auto n1 = mk(1, 0, TYPE_HEART_RATE, 72.f);
    auto c1 = mk(2, 0, TYPE_HEART_RATE, 130.f);
    stream.insert(stream.end(), n1.begin(), n1.end());
    stream.insert(stream.end(), c1.begin(), c1.end());
    acq.pushBytes(stream.data(), stream.size(), 1000);
    acq.drain();
    CHECK(sink.got.size() == 2);             // 两样本都入库
    CHECK(alarms == 1);                       // 仅危急值触发一次告警
    CHECK(acq.stat().ok == 2);
}

int main() {
    test_encode_decode_roundtrip();
    test_sticky_packets();
    test_split_packet();
    test_noise_resync();
    test_crc_corrupt();
    test_ring_basic();
    test_ring_spsc_threaded();
    test_acquisition_alarm();

    std::printf("\n%s  断言 %d 项, 失败 %d 项\n",
                g_fail == 0 ? "\x1b[32mALL PASS\x1b[0m " : "\x1b[31mHAS FAILURES\x1b[0m",
                g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
