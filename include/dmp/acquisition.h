// dmp/acquisition.h — 采集流水线 (骨架)
//
// 数据流: 原始字节 -> FrameDecoder 解帧 -> RingBuffer 解耦 -> Sink(落库/告警/UI)
// 这里定义 Sink 抽象接口 + 一个阈值告警检查器。落库/CSV 由具体实现(如 Qt 侧或独立服务)提供。
#pragma once

#include "dmp/frame_protocol.h"
#include "dmp/ring_buffer.h"

#include <functional>
#include <string>
#include <vector>

namespace dmp {

// 落库/推送的抽象终点, 便于替换 SQLite / gRPC / WebSocket / 文件
class Sink {
public:
    virtual ~Sink() = default;
    virtual void onSample(const Sample& s) = 0;
    virtual void flush() {}
};

// 阈值告警规则 (演示危急值; 真实设备按临床/工艺阈值配置)
struct AlarmRule {
    SampleType type;
    float      low  = -1e30f;
    float      high =  1e30f;
    std::string message;
};

struct AlarmEvent {
    Sample      sample;
    std::string message;
    bool        critical = false;
};

// 采集管道: 持有解码器 + 环形缓冲, 提供 pushBytes(热路径) 与 drain(消费)
class Acquisition {
public:
    explicit Acquisition(size_t ringCap = 8192) : ring_(ringCap) {}

    void addRule(AlarmRule r) { rules_.push_back(std::move(r)); }
    void setSink(Sink* s) { sink_ = s; }
    void setAlarmCallback(std::function<void(const AlarmEvent&)> cb) {
        onAlarm_ = std::move(cb);
    }

    // 生产者线程调用: 收到任何原始字节都喂进来
    void pushBytes(const uint8_t* data, size_t len, uint64_t ts_ms) {
        decoder_.feed(data, len, [&](Sample s) {
            s.recv_ts = ts_ms;
            if (!ring_.push(s)) ring_.addDropped(); // 满: 计数, 真实系统应背压
        });
    }

    // 消费者线程(UI/落库)调用: 批量取出并处理, 返回本轮样本数
    size_t drain(size_t maxBatch = 512) {
        if (!sink_) return 0;
        size_t n = 0;
        Sample s;
        while (n < maxBatch && ring_.pop(s)) {
            checkAlarm(s);
            sink_->onSample(s);
            ++n;
        }
        return n;
    }

    FrameDecoder::Stat stat() const { return decoder_.stat(); }
    uint64_t dropped() const { return ring_.dropped(); }   // 环形缓冲满丢弃计数

private:
    void checkAlarm(const Sample& s) {
        for (const auto& r : rules_) {
            if (r.type != s.type) continue;
            if (s.value < r.low || s.value > r.high) {
                if (onAlarm_) onAlarm_({s, r.message, true});
            }
        }
    }

    FrameDecoder           decoder_;
    RingBuffer<Sample>     ring_;
    std::vector<AlarmRule> rules_;
    Sink*                  sink_ = nullptr;
    std::function<void(const AlarmEvent&)> onAlarm_;
};

} // namespace dmp
