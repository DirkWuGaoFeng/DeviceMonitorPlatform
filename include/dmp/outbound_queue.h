// include/dmp/outbound_queue.h — 每客户端有界出向队列（纯逻辑，不碰 socket，可单测）
//
// 为什么存在（对应风险 R-009 / 需求 SR-014，详 docs/网关与服务链路优化方案.md T1.1）：
// 网关的广播是在**共享的单线程事件循环**里对阻塞 socket 直接 send 的。一个不读的公告客户端
// 会把内核发送缓冲填满，于是 send 停在半途 → 整个循环停摆 → 上游再没人读 → 真丢字节，
// 而且症状出现在离根因极远的地方（客户端只看到"数据变慢"）。
//
// 本模块把"生产"与"发送能力"解耦：广播只负责往队列里塞（不阻塞、有上界），
// 每轮循环再从队列里"能写多少写多少"。于是慢客户端的后果被限制在它自己的队列上。
//
// 三条不可让步的语义（都有对应断言，tests/test_outbound_queue.cpp）：
//   1) 有界：bytes 永不超过 capBytes；塞不下就丢，不 grow。
//   2) 按类型丢：Sample < Alert < Command 的保留优先级；丢的是**最旧**的可丢项，
//      次序保持（下游靠 seq 检核缺口，不靠重排）。Command 不可丢 —— 腾不出位就要求踢除。
//   3) 部分写友好：peek(n) 只看不删，commit(n) 才真正弹出；send 返回半行是常态不是错误。
//   4) 字节流完整性（Params::keepStreamIntact，给 RAW 订阅者）：开了这道开关就**不挤位**，
//      装不下只报 kickNeeded——宁可整条重连，也不在帧中间打洞。
#ifndef DMP_OUTBOUND_QUEUE_H
#define DMP_OUTBOUND_QUEUE_H

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

namespace dmp {
namespace obq {

// 保留优先级：数值大 = 更不该被丢。
enum class Kind { Sample = 0, Alert = 1, Command = 2 };

inline int rank(Kind k) { return static_cast<int>(k); }

struct Params {
    size_t capBytes     = 64 * 1024;   // 硬上限：队列字节数绝不超过
    size_t hiWaterBytes = 48 * 1024;   // 高水位：越过即进入 stalled（开始"降级"）
    size_t loWaterBytes = 16 * 1024;   // 低水位：回落到此以下才清除 stalled（滞回，防在边界抖）
    // 字节流完整性策略（对应方案 T1.1 定案 B-2，给 RAW 订阅者用）：文本行丢一行不影响后面的行，
    // 而字节流里挤掉任意一块都会在帧中间打洞——下游只能靠帧头重同步，并把 crc_err 顶成尖峰，
    // 污染的正是要做的归因。置 true 后本队列**从不挤掉已入队的内容**：装不下就 kickNeeded++。
    bool   keepStreamIntact = false;
};

struct Counters {
    uint64_t enqLines   = 0;  // 成功入队的行数
    uint64_t enqBytes   = 0;
    uint64_t sentBytes  = 0;  // commit 确认已发的字节（半行也算，故不配行计数）
    uint64_t dropLines  = 0;  // 被丢的行（被挤掉的旧行 + 因超长被拒的新行都算在这里）
    uint64_t dropBytes  = 0;
    uint64_t evictLines = 0;  // 为塞进新行而主动挤掉的旧行（dropLines 的子集）
    uint64_t evictBytes = 0;  // 同上子集的字节 —— 这些行**确实进过队列**，与被拒的行不同
    uint64_t kickNeeded = 0;  // "腾不出位且不可丢" 的次数 —— 调用方据此踢除该客户端
    // 两条不变量（任一侧漏计就会在长跑里漂掉）：
    //   enqBytes == 当前 bytes() + evictBytes + sentBytes   （只算真正进过队的字节）
    //   dropBytes == evictBytes + 被拒字节                 （被拒的从不计入 enqBytes）
};

class OutboundQueue {
public:
    // 不用 explicit：带默认参数的它同时充当默认构造函数，而 `Client nc{}` 这类
    // 聚合初始化会因 explicit 报 warn（g++ 13: "converting to ... from initializer list"）。
    // Params 是本模块独有的结构体，不存在误隐式转的风险。
    OutboundQueue(Params p = Params()) : p_(p) {}

    // 入队一行（约定含结尾 '\n'）。返回 true = 该行已在队列中。
    // 返回 false 只有两种情况：① 单行本身长到 capBytes 都装不下（配置/上游错误，丢该行）；
    // ② 需要腾位但腾不出（队列全是同级或更高级的不可丢项）—— 此时 kickNeeded++，
    //   由调用方按客户端类型决定"踢除"还是"置 stalled 继续留着连接"（本模块不做取舍）。
    bool push(const std::string& line, Kind k) {
        if (line.empty()) return true;                       // 空行不占位也不计数
        if (line.size() > p_.capBytes) {                     // ① 单行挤爆：丢它，队列原样不动
            ++c_.dropLines; c_.dropBytes += line.size();
            // 整块装不下对字节流而言是“必定的洞”：与其发一段中间缺字的流，不如要求踢除
            // （有重连能力的下游会从整帧重新对齐）。
            if (p_.keepStreamIntact) ++c_.kickNeeded;
            updateWater();
            return false;
        }
        // 腾位：从最旧开始找"保留优先级不高于 k"的项丢掉。
        // Command 的候选含 Sample/Alert，但绝不能含 Command 自己（应答丢了下游会误判链路）。
        const bool dropSameRank = (rank(k) < rank(Kind::Command));
        while (bytes_ + line.size() > p_.capBytes) {
            if (p_.keepStreamIntact) {                       // ②' 字节流：不挤，直接要求踢除
                ++c_.kickNeeded;
                updateWater();
                return false;
            }
            if (!evictOne(k, dropSameRank)) {                // ② 腾不出位
                ++c_.kickNeeded;
                updateWater();
                return false;
            }
        }
        q_.push_back(Item{line, k});
        bytes_ += line.size();
        ++c_.enqLines; c_.enqBytes += line.size();
        updateWater();
        return true;
    }

    // 取队首最多 maxBytes 字节去发送。只看不删 —— 发送失败可以原样再来。
    std::string peek(size_t maxBytes) const {
        std::string out;
        for (const auto& it : q_) {
            if (out.size() + it.s.size() <= maxBytes) { out += it.s; }
            else {
                size_t take = maxBytes > out.size() ? maxBytes - out.size() : 0;
                out.append(it.s, 0, take);
                break;                                        // 半行：到这里为止
            }
        }
        return out;
    }

    // 确认已发送 n 字节（n 不得超过上次 peek 的长度；超过部分被裁到实际字节数）。
    // n == 0 是合法的空转（EWOULDBLOCK 时就该传 0），不得改变任何状态。
    void commit(size_t n) {
        while (n > 0 && !q_.empty()) {
            size_t take = n < q_.front().s.size() ? n : q_.front().s.size();
            q_.front().s.erase(0, take);
            bytes_ -= take;
            c_.sentBytes += take;
            n -= take;
            if (q_.front().s.empty()) q_.pop_front();
        }
        updateWater();
    }

    bool   empty()  const { return q_.empty(); }
    size_t bytes()  const { return bytes_; }
    size_t lines()  const { return q_.size(); }
    bool   stalled() const { return stalled_; }
    const Counters& counters() const { return c_; }

    // 策略可后置开启：RAW 订阅发生在连接建立**之后**（客户端先 connect 再发 "RAW"），
    // 而重建队列会把累计计数清零——CLIENTS 报的是累计量，不能中途清。
    void requireStreamIntact(bool on) { p_.keepStreamIntact = on; }
    bool streamIntact() const { return p_.keepStreamIntact; }

    // 字节守恒：入队过的 == 还在队的 + 被挤掉的 + 已确认发出的。
    // 注意用 evictBytes 而非 dropBytes：超长行被拒时从未入队，把它算进来会凭空捏造字节。
    bool bytesBalanced() const {
        return c_.enqBytes == bytes_ + c_.evictBytes + c_.sentBytes;
    }

    // 不变量自检：累计字节 == 各行长度之和（单测用来抓"计数与内容脱钩"）
    size_t sumLineBytes() const {
        size_t t = 0;
        for (const auto& it : q_) t += it.s.size();
        return t;
    }

private:
    struct Item { std::string s; Kind k; };

    bool evictOne(Kind forK, bool dropSameRank) {
        for (size_t i = 0; i < q_.size(); ++i) {
            const int r = rank(q_[i].k);
            if (r < rank(forK) || (dropSameRank && r == rank(forK))) {
                bytes_ -= q_[i].s.size();
                ++c_.dropLines; c_.dropBytes += q_[i].s.size();
                ++c_.evictLines; c_.evictBytes += q_[i].s.size();
                q_.erase(q_.begin() + static_cast<long>(i));
                return true;
            }
        }
        return false;
    }

    // 滞回：只有明确越过 / 明确回落才改状态，中间的带子保持原状。
    void updateWater() {
        if (!stalled_) { if (bytes_ > p_.hiWaterBytes) stalled_ = true; }
        else           { if (bytes_ < p_.loWaterBytes) stalled_ = false; }
    }

    Params  p_;
    Counters c_{};
    std::deque<Item> q_;
    size_t bytes_ = 0;   // 当前在队字节数（不是累计量，故不进 Counters）
    bool stalled_ = false;
};

}  // namespace obq
}  // namespace dmp
#endif
