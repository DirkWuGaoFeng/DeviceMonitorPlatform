// tests/test_pipeline.cpp — 多线程采集流水线验证 (真 std::thread, 无三方框架)
//
// 目标: 证明 ProducerThread(生产: pushBytes) 与 主线程(消费: drain) 跨线程协作下,
//       经 SPSC 环形缓冲传输的样本"不丢、不重、不乱序丢失", 且分块喂入(每块 7 字节,
//       非帧长 12 的约数)能在流式解码器下正确重组粘包/拆包。
// 对应 IEC 可追溯 SR-002「采集/UI 解耦不丢帧」的多线程形态。
#include "dmp/pipeline.h"
#include "dmp/frame_protocol.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <set>
#include <thread>
#include <vector>

static int g_fail = 0;
static int g_run  = 0;
#define CHECK(cond) \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp;

// 收集端 (仅主线程经 drain 回调, 无需加锁)
struct CountingSink : public Sink {
    std::vector<uint16_t> seqs;
    void onSample(const Sample& s) override { seqs.push_back(s.seq); }
    size_t n() const { return seqs.size(); }
};

static void test_producer_consumer() {
    SECTION("生产/消费跨线程: N=5000 帧, 每块 7 字节喂入, 不丢不重");
    const int N = 5000;

    // 预生成 N 帧的连续字节流
    std::vector<uint8_t> stream;
    for (int i = 0; i < N; ++i) {
        Sample s;
        s.seq = static_cast<uint16_t>(i & 0xFFFF);
        s.channel = static_cast<uint8_t>(i % 4);
        s.type = static_cast<SampleType>((i % 4) + 1);
        s.value = 20.f + (i % 50);
        auto f = encodeFrame(s);
        stream.insert(stream.end(), f.begin(), f.end());
    }

    // 数据源: 每次拷至多 7 字节; 拷完后返回 0 空转几轮再 -1 结束 (仅生产者线程访问 pos)
    size_t pos = 0; int idle = 0;
    auto source = [&](void* buf, size_t cap) -> int {
        if (pos >= stream.size()) { if (++idle > 3) return -1; return 0; }
        size_t take = cap < 7 ? cap : 7;               // 故意非 12 的约数, 压测拆包
        if (pos + take > stream.size()) take = stream.size() - pos;
        std::memcpy(buf, stream.data() + pos, take);
        pos += take;
        return static_cast<int>(take);
    };

    Acquisition acq;
    CountingSink sink;
    acq.setSink(&sink);

    ProducerThread<decltype(source)> prod(acq, source);
    prod.start();

    // 主线程: 周期 drain, 直到收满或生产结束且缓冲排空
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (std::chrono::steady_clock::now() < deadline) {
        acq.drain(512);
        if (sink.n() >= (size_t)N) break;
        if (prod.finished() && acq.drain(512) == 0 && sink.n() >= (size_t)N) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    prod.stop();
    acq.drain(4096);                                    // 最终排空

    CHECK(sink.n() == (size_t)N);                        // 不丢不重(总数)
    CHECK(prod.ok() == (uint64_t)N);
    CHECK(prod.crcErr() == 0);
    CHECK(prod.dropped() == 0);
    std::set<uint16_t> uniq(sink.seqs.begin(), sink.seqs.end());
    CHECK(uniq.size() == (size_t)N);                     // 每个 seq 恰好一次
    // 顺序性: drain 保持入队顺序 => 收到的 seq 序列应为 0,1,2,...(按 16 位回绕)
    bool ordered = true;
    for (size_t i = 0; i < sink.seqs.size(); ++i)
        if (sink.seqs[i] != static_cast<uint16_t>(i & 0xFFFF)) { ordered = false; break; }
    CHECK(ordered);
}

static void test_stop_joinable() {
    SECTION("停止/回收: 数据源结束后线程可安全 join, 快照保持最终值");
    std::vector<uint8_t> one;
    Sample s; s.seq = 7; s.channel = 0; s.type = TYPE_TEMPERATURE; s.value = 1.f;
    auto f = encodeFrame(s); one = f;
    size_t pos = 0;
    auto source = [&](void* buf, size_t cap) -> int {
        if (pos >= one.size()) return -1;
        size_t take = one.size() - pos < cap ? one.size() - pos : cap;
        std::memcpy(buf, one.data() + pos, take); pos += take;
        return (int)take;
    };
    Acquisition acq; CountingSink sink; acq.setSink(&sink);
    {
        ProducerThread<decltype(source)> prod(acq, source);
        prod.start();
        for (int i = 0; i < 200 && sink.n() < 1; ++i) { acq.drain(64); std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
        CHECK(sink.n() == 1);
        CHECK(prod.ok() == 1);
    }   // 析构自动 stop()->join()
    CHECK(true);   // 走到这里说明 join 未死锁/未崩溃
}

int main() {
    test_producer_consumer();
    test_stop_joinable();
    std::printf("\nPIPELINE: run=%d fail=%d -> %s\n", g_run, g_fail, g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
