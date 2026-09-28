// dmp/ring_buffer.h — 有界 MPSC/SPSC 环形缓冲 (骨架)
//
// 作用: 把"采集/解码线程"(生产者) 与 "UI/落库线程"(消费者) 解耦,
//       避免在热路径上加锁, 保证高吞吐下不丢帧(满时可背压或丢弃并计数)。
//
// 说明: 这里给一个单生产者单消费者(SPSC)的无锁实现, 足以演示采集链路。
//       若多设备多线程写入, 换成带互斥的批次提交或每设备一个 ring + 聚合。
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <new>
#include <vector>

namespace dmp {

template <class T>
class RingBuffer {
public:
    explicit RingBuffer(size_t capacityPow2)
        : mask_(roundUpPow2(capacityPow2) - 1), buf_(roundUpPow2(capacityPow2)) {}

    // 生产者: 成功入队返回 true; 满返回 false (调用方决定背压/丢弃)
    bool push(const T& item) {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & mask_;
        if (next == tail_.load(std::memory_order_acquire)) return false; // 满
        buf_[head] = item;
        head_.store(next, std::memory_order_release);
        return true;
    }

    // 消费者: 取一个返回 true; 空返回 false
    bool pop(T& out) {
        const size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) return false; // 空
        out = buf_[tail];
        tail_.store((tail + 1) & mask_, std::memory_order_release);
        ++popped_;
        return true;
    }

    size_t capacity() const { return mask_ + 1; }
    uint64_t dropped() const { return dropped_; }
    void addDropped(uint64_t n = 1) { dropped_ += n; }

private:
    static size_t roundUpPow2(size_t n) {
        size_t p = 1; while (p < n) p <<= 1; return p;
    }

    size_t mask_;
    std::vector<T> buf_;
    std::atomic<size_t> head_{0};   // 写指针(生产者)
    std::atomic<size_t> tail_{0};   // 读指针(消费者)
    uint64_t popped_ = 0;
    uint64_t dropped_ = 0;          // 非原子, 仅诊断用
};

} // namespace dmp
