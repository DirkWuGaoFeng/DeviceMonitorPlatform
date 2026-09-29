// tests/test_service_proto.cpp — 网关文本行协议纯函数单测 (无 socket)
#include "dmp/service_proto.h"

#include <cstdio>
#include <string>

static int g_fail = 0, g_run = 0;
#define CHECK(cond) \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp;
using namespace dmp::svc;

static void test_parse() {
    SECTION("parseCommand: 关键字/大小写/参数/回退");
    CHECK(parseCommand("SUBSCRIBE").kind == CmdKind::Subscribe);
    CHECK(parseCommand("RAW").kind == CmdKind::Raw);                     // 原始帧透传订阅(大小写不敏感)
    CHECK(parseCommand("  raw \r\n").kind == CmdKind::Raw);
    CHECK(parseCommand("HISTORY").kind == CmdKind::History);
    CHECK(parseCommand("HISTORY").arg == -1);                            // 缺省: 全部通道/60s
    CHECK(parseCommand("HISTORY").arg2 == 60);
    CHECK(parseCommand("history 2 120").arg == 2);
    CHECK(parseCommand("history 2 120").arg2 == 120);
    CHECK(parseCommand("history -3 x").arg == -1);                        // 非法回退默认
    CHECK(parseCommand("history -3 x").arg2 == 60);
    CHECK(parseCommand("  stats \r\n").kind == CmdKind::Stats);      // 去空白与 CRLF
    CHECK(parseCommand("CLIENTS").kind == CmdKind::Clients);      // 每连接出向队列体检
    CHECK(parseCommand("  clients \r\n").kind == CmdKind::Clients);   // 大小写不敏感 + 去空白
    CHECK(parseCommand("Help").kind == CmdKind::Help);
    CHECK(parseCommand("ALARMS 3").kind == CmdKind::Alarms);
    CHECK(parseCommand("ALARMS 3").arg == 3);
    CHECK(parseCommand("alarms").arg == 10);                          // 默认 limit
    CHECK(parseCommand("alarms 0").arg == 10);                        // 非法参数回退默认
    auto u = parseCommand("foo");
    CHECK(u.kind == CmdKind::Unknown);
    CHECK(u.name == "foo");                                           // 原样回显(已小写)
    CHECK(parseCommand("").kind == CmdKind::Help);                    // 空行当 help
}

static void test_rule_parse() {
    SECTION("RULE/RULES 解析: 类型别名/浮点/带空格文案/非法入参");
    CHECK(parseCommand("RULES").kind == CmdKind::Rules);           // 数字前缀不干扰复数形式
    auto r = parseCommand("RULE 2 40 120 hr too fast");
    CHECK(r.kind == CmdKind::SetRule);
    CHECK(r.arg == TYPE_HEART_RATE);                                // 字面数字
    CHECK(r.f1 == 40.f && r.f2 == 120.f);
    CHECK(r.text == "hr too fast");                                 // 余下整句含空格
    CHECK(parseCommand("rule HR 50 110").arg == TYPE_HEART_RATE);   // 名字形式 + 大小写不敏感
    CHECK(parseCommand("RULE temperature 35 42").arg == TYPE_TEMPERATURE);
    CHECK(parseCommand("RULE spo2 90 100").arg == TYPE_SPO2);
    CHECK(parseCommand("RULE conc 1 2").arg == TYPE_CONCENTR);
    CHECK(parseCommand("RULE press 1 2").arg == TYPE_PRESSURE);
    CHECK(parseCommand("RULE hr 50 110").text.empty());             // 无文案 => 空串(服务端给默认)
    auto neg = parseCommand("RULE hr -5 5");
    CHECK(neg.kind == CmdKind::SetRule && neg.f1 == -5.f);          // 负阈值合法词法
    CHECK(parseCommand("RULE BOGUS 1 2").kind == CmdKind::Unknown); // 未知类型名
    CHECK(parseCommand("RULE BOGUS 1 2").name == "rule <bad type: BOGUS>");
    CHECK(parseCommand("RULE hr 50").kind == CmdKind::Unknown);        // 缺 high
    CHECK(parseCommand("RULE hr abc 60").kind == CmdKind::Unknown);    // 非数值
    CHECK(parseCommand("RULE").kind == CmdKind::Unknown);              // 缺全部参数
    CHECK(typeFromWord("hr") == TYPE_HEART_RATE && typeFromWord("xx") == 0);
    CHECK(typeFromWord("6") == 0 && typeFromWord("0") == 0);           // 越界编号一律拒绝
}

static void test_format() {
    SECTION("format*: 逐字节精确输出");
    CHECK(formatStats(5, 1, 2) == "STATS ok=5 crc_err=1 dropped=2\n");
    CHECK(formatError("bad cmd") == "ERR bad cmd\n");
    CHECK(formatHelp() == "COMMANDS subscribe raw history[ch][sec] stats alarms[limit] "
                          "rules rule[type][low][high][msg] clients help\n");

    // CLIENT 行：字段顺序与名字都是下游正则的一部分，逐字节钉住
    ClientStat cs; cs.id = 3; cs.sub = 1; cs.raw = 0; cs.qbytes = 4096; cs.lines = 58;
    cs.stalled = 1; cs.enqLines = 1000; cs.sentBytes = 61040;
    cs.dropLines = 100; cs.evictLines = 96; cs.kickLines = 4;
    CHECK(formatClient(cs) == "CLIENT id=3 sub=1 raw=0 qbytes=4096 lines=58 stalled=1 "
                              "enq=1000 sent=61040 drop=100 evict=96 kick=4\n");
    ClientStat z;   // 空初值也必须逐字段给 0，不得出现空字段或未初始化数字
    CHECK(formatClient(z) == "CLIENT id=0 sub=0 raw=0 qbytes=0 lines=0 stalled=0 "
                             "enq=0 sent=0 drop=0 evict=0 kick=0\n");
    // 缓冲够不够只能算一遍：一切数字取 uint64 满值时若内部缓冲偏小，
    // 被截掉的恰好是最后的 kick= —— 下游会读到一个被缩短的数字而不是报错。
    ClientStat big = z;
    big.sub = big.raw = 1;
    big.id = big.qbytes = big.lines = big.stalled = big.enqLines =
    big.sentBytes = big.dropLines = big.evictLines = big.kickLines = 18446744073709551615ull;
    CHECK(formatClient(big).size() == 257);
    CHECK(formatClient(big).find("kick=18446744073709551615\n") != std::string::npos);
    CHECK(formatClient(big).back() == '\n');

    HistAgg ha; ha.channel = 3; ha.count = 12; ha.avg = 5.5; ha.min = 1.25; ha.max = 9.75; ha.last = 4.0;
    CHECK(formatHistory(ha) == "HISTORY ch=3 count=12 avg=5.500 min=1.250 max=9.750 last=4.000\n");

    Sample s; s.seq = 7; s.channel = 2; s.type = TYPE_HEART_RATE; s.value = 72.5f; s.recv_ts = 1000;
    CHECK(formatSample(s) == "SAMPLE ts=1000 seq=7 ch=2 type=2 name=HR value=72.500\n");

    AlarmEvent a; a.sample = s; a.sample.recv_ts = 2000; a.message = "HR high"; a.critical = true;
    CHECK(formatAlarm(a) == "ALARM ts=2000 ch=2 type=2 name=HR value=72.500 critical=1 msg=HR high\n");

    AlarmRule rr; rr.type = TYPE_HEART_RATE; rr.low = 40; rr.high = 120; rr.message = "hr too fast";
    CHECK(formatRule(rr) == "RULE type=2 name=HR low=40.000 high=120.000 msg=hr too fast\n");
    AlarmRule rn; rn.type = TYPE_SPO2; rn.low = 90; rn.high = 100;   // 空文案以 - 占位, 保证字段不断行
    CHECK(formatRule(rn) == "RULE type=3 name=SPO2 low=90.000 high=100.000 msg=-\n");
    CHECK(formatRuleAck(rr) == "RULE_ACK accepted=1 RULE type=2 name=HR low=40.000 high=120.000 msg=hr too fast\n");
}

// 配置面语义: 下发幂等 + 下发确实改变告警行为 (写下去要能看到行为差, 否则只是自娱自乐)
static void test_rule_apply() {
    SECTION("Acquisition 规则: 同类型覆盖幂等/删除/下发后行为改变");
    Acquisition acq;
    acq.addRule({TYPE_HEART_RATE, 50.f, 110.f, "HR out of range"});
    CHECK(acq.rules().size() == 1);
    acq.setRule({TYPE_HEART_RATE, 40.f, 120.f, "tight"});        // 覆盖而非追加
    CHECK(acq.rules().size() == 1);
    CHECK(acq.rules()[0].low == 40.f && acq.rules()[0].message == "tight");
    acq.setRule({TYPE_HEART_RATE, 40.f, 120.f, "tight"});        // 重复下发无副作用
    CHECK(acq.rules().size() == 1);
    acq.setRule({TYPE_TEMPERATURE, 35.f, 42.f, "temp"});         // 不同类型才追加
    CHECK(acq.rules().size() == 2);
    CHECK(acq.removeRule(TYPE_TEMPERATURE) == true);
    CHECK(acq.removeRule(TYPE_TEMPERATURE) == false);            // 再删不存在 => false
    CHECK(acq.rules().size() == 1);

    // 行为差: 72.5bpm 在原阈值(50..110)不告警, 收紧到 40..70 必须告警
    int fired = 0;
    acq.setAlarmCallback([&](const AlarmEvent&) { ++fired; });
    struct Cap : Sink {
        std::vector<Sample> v; void onSample(const Sample& s) override { v.push_back(s); }
    } cap;
    acq.setSink(&cap);
    const auto bytes = encodeFrame([]{ Sample s; s.seq = 1; s.channel = 0; s.type = TYPE_HEART_RATE; s.value = 72.5f; return s; }());
    acq.pushBytes(bytes.data(), bytes.size(), 1000);
    acq.drain();
    CHECK(fired == 0);                                           // 阈值内: 不应告警
    acq.setRule({TYPE_HEART_RATE, 40.f, 70.f, "HR low now"});    // 下发收紧
    acq.pushBytes(bytes.data(), bytes.size(), 1001);
    acq.drain();
    CHECK(fired == 1);                                           // 同一数据, 配置不通过 => 告警
    CHECK(acq.stat().ok == 2);                                   // 解码链路未被配置操作污染
}

int main() {
    test_parse();
    test_rule_parse();
    test_format();
    test_rule_apply();
    std::printf("\nSERVICE_PROTO: run=%d fail=%d -> %s\n", g_run, g_fail, g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
