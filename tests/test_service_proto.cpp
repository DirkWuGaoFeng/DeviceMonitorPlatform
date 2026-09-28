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

static void test_format() {
    SECTION("format*: 逐字节精确输出");
    CHECK(formatStats(5, 1, 2) == "STATS ok=5 crc_err=1 dropped=2\n");
    CHECK(formatError("bad cmd") == "ERR bad cmd\n");
    CHECK(formatHelp() == "COMMANDS subscribe raw history[ch][sec] stats alarms[limit] help\n");

    HistAgg ha; ha.channel = 3; ha.count = 12; ha.avg = 5.5; ha.min = 1.25; ha.max = 9.75; ha.last = 4.0;
    CHECK(formatHistory(ha) == "HISTORY ch=3 count=12 avg=5.500 min=1.250 max=9.750 last=4.000\n");

    Sample s; s.seq = 7; s.channel = 2; s.type = TYPE_HEART_RATE; s.value = 72.5f; s.recv_ts = 1000;
    CHECK(formatSample(s) == "SAMPLE ts=1000 seq=7 ch=2 type=2 name=HR value=72.500\n");

    AlarmEvent a; a.sample = s; a.sample.recv_ts = 2000; a.message = "HR high"; a.critical = true;
    CHECK(formatAlarm(a) == "ALARM ts=2000 ch=2 type=2 name=HR value=72.500 critical=1 msg=HR high\n");
}

int main() {
    test_parse();
    test_format();
    std::printf("\nSERVICE_PROTO: run=%d fail=%d -> %s\n", g_run, g_fail, g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
