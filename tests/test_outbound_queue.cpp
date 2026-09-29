// tests/test_outbound_queue.cpp — 每客户端有界出向队列的语义验证（对应 SR-014 / R-009）
//
// 这里验的是**机制**不是效果：慢客户端被隔离是网关集成后的实测（tools/diagnostics/...），
// 而"有界、按类型丢、部分写友好、水位滞回"这四条若在任何一条上写错，集成侧就会
// 要么丢不该丢的字节，要么在边界上抖，要么把不可丢的应答丢掉。所以每条都配了能证伪的格子。
#include "dmp/outbound_queue.h"

#include <cstdio>
#include <string>

static int g_fail = 0;
static int g_run  = 0;
#define CHECK(cond) \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp::obq;

// 造一条恰好 n 字节的行（末尾 '\n'），首字符是 tag 便于验证"留下的是哪一条"
static std::string lineN(char tag, size_t n) {
    std::string s(1, tag);
    while (s.size() + 1 < n) s += '.';
    s += '\n';
    return s;
}

static Params smallP(size_t cap, size_t hi, size_t lo) {
    Params p; p.capBytes = cap; p.hiWaterBytes = hi; p.loWaterBytes = lo; return p;
}

static void test_basics() {
    SECTION("基础：空行不占位、字节与行内容始终一致");
    OutboundQueue q(smallP(100, 80, 20));
    CHECK(q.empty());
    CHECK(q.bytes() == 0);
    CHECK(q.push("", Kind::Sample));          // 空行：接纳但不计数
    CHECK(q.bytes() == 0 && q.lines() == 0);
    CHECK(q.counters().enqLines == 0);
    CHECK(q.push(lineN('A', 30), Kind::Sample));
    CHECK(q.bytes() == 30 && q.lines() == 1);
    CHECK(q.sumLineBytes() == q.bytes());     // 不变量：计数与内容不脱钩
    CHECK(q.push(lineN('B', 30), Kind::Sample));
    CHECK(q.counters().enqLines == 2 && q.counters().enqBytes == 60);
    CHECK(q.peek(1).empty() == false && q.peek(1)[0] == 'A');   // FIFO：最旧的先走
    CHECK(q.counters().sentBytes == 0);            // 没 commit 过就不该有"已发"
    CHECK(q.bytesBalanced());
}

static void test_bounded_evicts_oldest_same_kind() {
    SECTION("有界：满时丢**最旧**的同级样本，且 bytes 永不越上限");
    OutboundQueue q(smallP(100, 80, 20));
    for (char t = '1'; t <= '5'; ++t) q.push(lineN(t, 30), Kind::Sample);
    CHECK(q.bytes() == 90);                    // 90 = 3 条 × 30，不是 150 也不是 120
    CHECK(q.bytes() <= 100);
    CHECK(q.lines() == 3);
    CHECK(q.counters().enqLines == 5);
    CHECK(q.counters().dropLines == 2);        // 丢了两条最旧的
    CHECK(q.counters().evictLines == 2);       // 且都是"为腾位挤掉的"
    CHECK(q.counters().evictBytes == 60);      // 这一格无被拒行，故 dropBytes == evictBytes
    CHECK(q.counters().kickNeeded == 0);       // 样本之间腾得出来，不该要求踢除
    CHECK(q.sumLineBytes() == q.bytes());
    // 留下的是最新的三条：L3 L4 L5
    std::string all = q.peek(1000);
    CHECK(all.size() == 90);
    CHECK(all[0] == '3' && all[30] == '4' && all[60] == '5');
    // 丢掉的字节数应等于两条 30 字节的行
    CHECK(q.counters().dropBytes == 60);
    // 字节守恒（注意不是 enq+drop==喂入量：被驱逐的行**曾经入队**，两边都计）：
    //   入队总量 == 现存 + 被丢量   →   5×30 = 90 + 60
    CHECK(q.counters().enqBytes == 150);
    CHECK(q.counters().enqBytes == q.bytes() + q.counters().dropBytes);   // 本场景无 commit，sentBytes 必为 0
    CHECK(q.counters().sentBytes == 0);
    CHECK(q.bytesBalanced());
}

static void test_priority_keeps_command() {
    SECTION("保留优先级：Command 可以挤掉样本/告警，但绝不挤掉 Command");
    OutboundQueue q(smallP(100, 80, 20));
    for (int i = 0; i < 3; ++i) q.push(lineN('A', 30), Kind::Alert);   // 三条告警占 90
    CHECK(q.bytes() == 90 && q.lines() == 3);
    // 命令应答进来：腾位应丢最旧的告警（rank 1 < 2），应答本身留下
    for (int i = 0; i < 3; ++i) CHECK(q.push(lineN('C', 30), Kind::Command));
    CHECK(q.counters().dropLines == 3);        // 三条告警被挤掉
    CHECK(q.counters().kickNeeded == 0);
    CHECK(q.lines() == 3 && q.bytes() == 90);
    // 第四条命令：队列里已无更低优先级可丢 → 不许丢命令，要报"需要踢除"
    CHECK(!q.push(lineN('X', 30), Kind::Command));
    CHECK(q.counters().kickNeeded == 1);
    CHECK(q.bytes() == 90 && q.lines() == 3);  // 拒绝必须是**无副作用**的
    CHECK(q.peek(1)[0] == 'C');                // 前三条命令仍在，且没有被新行污染
    // 被拒的第四条不算丢弃、只算 kickNeeded：所以入队量与丢弃量能严格对账
    CHECK(q.counters().enqBytes == 180);
    CHECK(q.counters().dropBytes == 90);        // 三条告警被挤掉
    CHECK(q.counters().enqBytes == q.bytes() + q.counters().dropBytes);   // 180 = 90 + 90
    CHECK(q.bytesBalanced());
}

static void test_partial_send() {
    SECTION("部分写友好：peek 不删、commit 才删、半行是常态、n=0 是合法空转");
    OutboundQueue q(smallP(100, 80, 20));
    q.push("ABCD\n", Kind::Sample);            // 5 字节
    q.push("EFGHIJ\n", Kind::Sample);          // 7 字节
    CHECK(q.bytes() == 12 && q.lines() == 2);
    std::string w = q.peek(7);                 // 只给 7 字节的发送预算
    CHECK(w == "ABCD\nEF");                    // 跨行边界，正好切在半行
    CHECK(q.bytes() == 12 && q.lines() == 2);  // 关键：peek 之后状态一点没动（可重发）
    q.commit(7);
    CHECK(q.bytes() == 5 && q.lines() == 1);
    CHECK(q.counters().sentBytes == 7);            // 已发字节只按真正弹走的量计
    CHECK(q.peek(100) == "GHIJ\n");            // 剩下的半行仍在队首，续得上
    q.commit(0);                               // EWOULDBLOCK 时的空转
    CHECK(q.bytes() == 5 && q.lines() == 1 && q.counters().enqLines == 2);
    CHECK(q.counters().sentBytes == 7);            // 空转不许让"已发"前进（否则下游对不上账）
    q.commit(999);                             // 过度确认：裁到实际字节，不返回负数
    CHECK(q.empty() && q.bytes() == 0);
    CHECK(q.counters().sentBytes == 12);           // 12 = 7 + 5，不得把 999 记成已发
    CHECK(q.peek(10).empty());                 // 空队列 peek 给空串而不是崩溃
    CHECK(q.sumLineBytes() == q.bytes());
    CHECK(q.bytesBalanced());                      // 12 = 0 + 0 + 12
}

static void test_water_hysteresis() {
    SECTION("水位滞回：越过 hi 才 stalled，回落到 lo 以下才清除（中间带保持原状）");
    OutboundQueue q(smallP(200, 80, 20));
    CHECK(!q.stalled());
    q.push(lineN('1', 30), Kind::Sample);
    q.push(lineN('2', 30), Kind::Sample);      // 60 < 80
    CHECK(!q.stalled());
    q.push(lineN('3', 30), Kind::Sample);      // 90 > 80
    CHECK(q.stalled());
    q.commit(20);                              // 回到 70：低于 hi 但高于 lo
    CHECK(q.bytes() == 70);
    CHECK(q.stalled());                        // ← 朴素实现（bytes>hi 就现算）会在这里挂
    q.commit(60);                              // 回到 10：低于 lo
    CHECK(!q.stalled());
    CHECK(q.bytesBalanced());
}

static void test_overlong_line() {
    SECTION("单行超过硬上限：丢这一行且队列原样不动（不许 grow，也不许清空别人）");
    OutboundQueue q(smallP(100, 80, 20));
    CHECK(q.push(lineN('A', 30), Kind::Sample));
    CHECK(!q.push(lineN('M', 101), Kind::Command));   // 一行就超上限
    CHECK(q.lines() == 1 && q.bytes() == 30);         // 原有内容未被破坏
    CHECK(q.counters().dropLines == 1 && q.counters().evictLines == 0);  // 是"被拒"不是"挤掉"
    CHECK(q.counters().dropBytes == 101);
    CHECK(q.push(lineN('B', 100), Kind::Sample));     // 恰好等于上限：合法边界
    // 这条 100 字节的行会把旧的 30 挤掉（同级可丢），然后自己正好填满 —— 上限是**含等于**的
    CHECK(q.bytes() == 100 && q.lines() == 1);
    CHECK(q.peek(1)[0] == 'B');                       // 留下的确实是新的那条，不是 A
    CHECK(q.counters().dropLines == 2);                // 一条被拒 + 一条被挤掉
    CHECK(q.counters().evictLines == 1);               // 其中只有 1 条是挤掉
    CHECK(q.counters().dropBytes == 131);              // 101（被拒）+ 30（被挤掉）
    CHECK(q.counters().evictBytes == 30);              // 只有被挤掉的那条真正进过队
    CHECK(q.counters().enqBytes == 130);               // 真正进过队列的只有 30 + 100
    CHECK(q.sumLineBytes() == q.bytes());
    CHECK(q.bytesBalanced());                          // 130 = 100 + 30 + 0（不能用 dropBytes=131，否则不闭合）
}

static void test_counters_monotonic() {
    SECTION("计数单调不减：commit/驱逐都不许让计数倒退");
    OutboundQueue q(smallP(120, 90, 30));
    uint64_t e0 = 0, d0 = 0;
    for (int i = 0; i < 40; ++i) {
        q.push(lineN(static_cast<char>('a' + i % 26), 20), Kind::Sample);
        if (i % 3 == 0) q.commit(q.peek(15).size());
        CHECK(q.counters().enqLines >= e0);
        CHECK(q.counters().dropLines >= d0);
        CHECK(q.bytes() <= 120);
        e0 = q.counters().enqLines; d0 = q.counters().dropLines;
    }
    CHECK(q.counters().enqLines == 40);               // 40 次调用全部入队成功（样本总是腾得出位）
    CHECK(q.counters().enqBytes == 800);              // 40 × 20，无一被吞
    CHECK(q.counters().kickNeeded == 0);              // 全程不该要求踢除，否则下面的对账不成立
    CHECK(q.counters().dropLines == q.counters().evictLines);  // 没有超长行，丢弃必全来自挤掉
    CHECK(q.counters().dropBytes == q.counters().evictBytes);   // 同理，字节侧也不得分离
    // 这一格每 3 次就 commit 一段 15 字节，所以守恒式里**必须算上已发量**：
    //   只写 enq == bytes + drop 会在第二个 commit 之后假报失败（本轮就是这么写错的）。
    //   发次数 = i∈{0,3,...,39} 共 14 次，每次恰 15 字节（队列非空时 peek(15) 总能拿满）。
    CHECK(q.counters().sentBytes == 210);
    // 按 cap=120 / 行 20 字节手推的终态：每个 i%3==0 之后回到 105 字节
    CHECK(q.bytes() == 105);
    CHECK(q.counters().enqBytes == q.bytes() + q.counters().dropBytes + q.counters().sentBytes);  // 800 = 105 + 485 + 210
    CHECK(q.sumLineBytes() == q.bytes());
    CHECK(q.bytesBalanced());
}

static void test_stream_intact() {
    SECTION("字节流完整性（RAW 开 keepStreamIntact）：不挤位，装不下只报 kickNeeded");
    OutboundQueue q(smallP(100, 80, 20));
    q.requireStreamIntact(true);
    CHECK(q.streamIntact());
    CHECK(q.push(lineN('A', 40), Kind::Sample));
    CHECK(q.push(lineN('B', 40), Kind::Sample));      // 80 <= 100，未碰上限
    CHECK(q.counters().kickNeeded == 0);              // 没撞上限时开关不该改变任何行为
    CHECK(!q.push(lineN('C', 40), Kind::Sample));     // 需要腾位 —— 但它不挤
    CHECK(q.counters().kickNeeded == 1);
    // 一块都没丢：开关的语义就是“要么原样留着，要么整条重连”，中间缺一块总是不可接受
    CHECK(q.counters().evictLines == 0 && q.counters().dropLines == 0);
    CHECK(q.bytes() == 80 && q.peek(1)[0] == 'A');    // 原样未动（被拒的是新到的那块，不是队里的一块）
    // 整块比上限还大：文本行那里是“静默拒收”，对字节流而言是必定的洞 → 也要报踢除
    CHECK(!q.push(lineN('L', 101), Kind::Sample));
    CHECK(q.counters().kickNeeded == 2);
    CHECK(q.counters().dropLines == 1 && q.counters().evictLines == 0);
    CHECK(q.bytesBalanced());                         // 80 = 80(在队) + 0 + 0
    // 关掉开关后回到文本行语义：同级样本可以互相挤
    q.requireStreamIntact(false);
    CHECK(q.push(lineN('D', 40), Kind::Sample));
    CHECK(q.counters().evictLines == 1 && q.bytes() == 80);
    CHECK(q.bytesBalanced());                         // 120 = 80 + 40(evict) + 0(sent)
}

int main() {
    test_basics();
    test_bounded_evicts_oldest_same_kind();
    test_priority_keeps_command();
    test_partial_send();
    test_water_hysteresis();
    test_overlong_line();
    test_counters_monotonic();
    test_stream_intact();
    std::printf("OUTBOUND-QUEUE: run=%d fail=%d -> %s\n",
                g_run, g_fail, g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
