// dmp/pipeline.h — 采集流水线线程化封装 (生产/消费分离)
//
// 背景: Acquisition 内建 SPSC 无锁环形缓冲, 天然要求"单生产者 pushBytes + 单消费者 drain"。
// 本封装把"从传输(串口/socket)阻塞读取原始字节"放到独立生产者线程, 主线程(UI/渲染)只做
// 周期性 drain —— 读盘的阻塞不再卡住界面刷新, 且仍严守 SPSC 的所有权边界:
//   生产者线程: 唯一调用 acq.pushBytes(); 并在同线程读 stat()/dropped() 做原子快照。
//   主线程    : 唯一调用 acq.drain(); 只读生产者发布的原子快照, 不碰解码器内部。
#pragma once

#include "dmp/acquisition.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace dmp {

// Source: 可调用对象 int(void* buf, size_t cap)
//   >0 读到 n 字节; 0 暂无数据(短暂让出 CPU); <0 数据源结束/错误 → 生产者线程退出。
template <class Source>
class ProducerThread {
public:
    ProducerThread(Acquisition& acq, Source src) : acq_(acq), src_(std::move(src)) {}
    ~ProducerThread() { stop(); }

    ProducerThread(const ProducerThread&) = delete;
    ProducerThread& operator=(const ProducerThread&) = delete;

    void start() {
        if (running_.exchange(true)) return;         // 已在跑
        stop_.store(false);
        th_ = std::thread([this] { loop(); });
    }

    void stop() {
        stop_.store(true);
        if (th_.joinable()) th_.join();
        running_.store(false);
    }

    // 主线程只读这些原子快照 (由生产者线程更新), 无数据竞争
    uint64_t ok()      const { return ok_.load(std::memory_order_relaxed); }
    uint64_t crcErr()  const { return crcErr_.load(std::memory_order_relaxed); }
    uint64_t dropped() const { return drop_.load(std::memory_order_relaxed); }
    bool     finished() const { return finished_.load(std::memory_order_relaxed); }

private:
    void loop() {
        std::vector<uint8_t> buf(4096);
        while (!stop_.load(std::memory_order_relaxed)) {
            int n = src_(buf.data(), buf.size());
            if (n < 0) break;
            if (n > 0) {
                acq_.pushBytes(buf.data(), static_cast<size_t>(n), nowMs());
                // 与 pushBytes 同线程读解码统计 → 安全, 再原子发布给主线程
                auto st = acq_.stat();
                ok_.store(st.ok, std::memory_order_relaxed);
                crcErr_.store(st.crcErr, std::memory_order_relaxed);
                drop_.store(acq_.dropped(), std::memory_order_relaxed);
            } else {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
        }
        // 收尾: 把最终统计发布一次
        auto st = acq_.stat();
        ok_.store(st.ok, std::memory_order_relaxed);
        crcErr_.store(st.crcErr, std::memory_order_relaxed);
        drop_.store(acq_.dropped(), std::memory_order_relaxed);
        finished_.store(true, std::memory_order_relaxed);
    }

    static uint64_t nowMs() {
        using namespace std::chrono;
        return static_cast<uint64_t>(
            duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count());
    }

    Acquisition&   acq_;
    Source         src_;
    std::thread    th_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> finished_{false};
    std::atomic<uint64_t> ok_{0}, crcErr_{0}, drop_{0};
};

// 便捷工厂: 从 Acquisition + Source 构造一个 ProducerThread (推导 Source 模板参数)
template <class Source>
ProducerThread<Source> makeProducer(Acquisition& acq, Source src) {
    return ProducerThread<Source>(acq, std::move(src));
}

} // namespace dmp
