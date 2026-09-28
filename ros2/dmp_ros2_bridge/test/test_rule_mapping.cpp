// tests(test)/test_rule_mapping.cpp — 桥内纯函数层断言 (无 ROS 运行时)
// 与仓库其他测试同一风格: CHECK 宏 + 末尾打印 run/fail, 便于 CI 统计断言数。
#include "dmp_ros2_bridge/rule_mapping.hpp"

#include <cstdio>
#include <string>

static int g_fail = 0, g_run = 0;
#define CHECK(cond) \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmpbr;

static void test_parse_rule_line() {
    SECTION("parseRuleLine: 正常/带空格文案/占位/杂散行/畸形");
    dmp::AlarmRule r;
    CHECK(parseRuleLine("RULE type=2 name=HR low=40.000 high=120.000 msg=hr too fast\n", &r));
    CHECK(r.type == dmp::TYPE_HEART_RATE);
    CHECK(r.low == 40.f && r.high == 120.f);
    CHECK(r.message == "hr too fast");                 // 文案含空格, 取到行尾
    CHECK(parseRuleLine("RULE type=3 name=SPO2 low=94.000 high=100.000 msg=-\n", &r));
    CHECK(r.type == dmp::TYPE_SPO2 && r.message == "SPO2 out of range");   // "-" 占位 -> 默认文案
    CHECK(parseRuleLine("RULE type=1 name=TEMP low=35.000 high=42.000 msg=体温越界", &r));
    CHECK(r.message == "体温越界");                     // 无行尾换行也要能解
    CHECK(!parseRuleLine("+SUBSCRIBED\n", &r));         // 杂散控制行
    CHECK(!parseRuleLine("RULE none\n", &r));           // 空配置应答
    CHECK(!parseRuleLine("STATS ok=1 crc_err=0 dropped=0\n", &r));
    CHECK(!parseRuleLine("RULE type=9 name=X low=1 high=2 msg=x\n", &r));  // 未知类型
    CHECK(!parseRuleLine("RULE type=2 name=HR low=40\n", &r));             // 缺 high
    CHECK(!parseRuleLine("RULE type=2 name=HR low=abc high=9\n", &r));     // 非数值
    CHECK(!parseRuleLine("RULE \n", nullptr));                             // 空指针防御
    dmp::AlarmRule bad;
    CHECK(!parseRuleLine("HISTORY ch=1 count=2 avg=1.0 min=1.0 max=1.0 last=1.0\n", &bad));
    CHECK(!parseRuleLine("RULE type=2 name=HR low=120 high=40 msg=x\n", &bad));   // 区间倒置
    CHECK(!parseRuleLine("RULE type=2 name=HR low=50 high=50 msg=x\n", &bad));    // 区间退化
    CHECK(!parseRuleLine("RULE type=2 name=HR low=40 high=1e999 msg=x\n", &bad)); // inf 不当合法阈值
}

static void test_float_strict() {
    SECTION("parseFloatStrict: 只接受整个 token 是数");
    float v = -1.f;
    CHECK(parseFloatStrict("40.000", &v) && v == 40.f);
    CHECK(parseFloatStrict("-5.5", &v) && v == -5.5f);
    CHECK(parseFloatStrict("0", &v) && v == 0.f);
    CHECK(!parseFloatStrict("abc", &v));
    CHECK(!parseFloatStrict("40abc", &v));
    CHECK(!parseFloatStrict("", &v));
    CHECK(!parseFloatStrict("nan", &v));                 // 非有限值不当配置
    CHECK(!parseFloatStrict("inf", &v));
    CHECK(!parseFloatStrict("3", nullptr));               // 空指针防御
}

static void test_level() {
    SECTION("levelFor: 越界=Error / 预警带=Warn / 带外沿=Ok / 退化区间不产生 Warn");
    dmp::AlarmRule hr; hr.type = dmp::TYPE_HEART_RATE; hr.low = 50.f; hr.high = 110.f;
    CHECK(levelFor(72.f, hr) == HealthLevel::Ok);          // 带中央
    CHECK(levelFor(49.9f, hr) == HealthLevel::Error);      // 越下界
    CHECK(levelFor(110.1f, hr) == HealthLevel::Error);     // 越上界
    CHECK(levelFor(50.f, hr) == HealthLevel::Error ||
          levelFor(50.f, hr) == HealthLevel::Warn);        // 边界本身不算越界, 但落在预警带
    CHECK(levelFor(51.f, hr) == HealthLevel::Warn);        // span=60, margin=3 => <53 预警
    CHECK(levelFor(109.f, hr) == HealthLevel::Warn);       // >107 预警
    CHECK(levelFor(53.1f, hr) == HealthLevel::Ok);
    CHECK(levelFor(72.f, hr, 0.f) == HealthLevel::Ok);     // 关掉预警带
    dmp::AlarmRule flat; flat.type = dmp::TYPE_SPO2; flat.low = 100.f; flat.high = 100.f;
    CHECK(levelFor(100.f, flat) == HealthLevel::Ok);       // 退化区间(span=0)不除零、不恒 Warn
    CHECK(levelFor(101.f, flat) == HealthLevel::Error);
}

static void test_stats_and_count() {
    SECTION("parseStatsLine / countPrefixedLines");
    StreamCounters c;
    CHECK(parseStatsLine("STATS ok=34 crc_err=0 dropped=0\n", &c));
    CHECK(c.ok == 34 && c.crcErr == 0 && c.dropped == 0);
    CHECK(!c.corrupted());
    CHECK(parseStatsLine("STATS ok=18446744073709551615 crc_err=2 dropped=3", &c));
    CHECK(c.crcErr == 2 && c.dropped == 3);
    CHECK(c.corrupted());                                   // 完整性受损判定
    CHECK(!parseStatsLine("SAMPLE ts=1 seq=2 ch=0 type=1 value=9.0\n", &c));
    CHECK(!parseStatsLine("STATS ok=1 crc_err=2\n", &c));   // 缺 dropped
    CHECK(!parseStatsLine("STATS ok= crc_err=1 dropped=2\n", &c));  // 空数值
    CHECK(countPrefixedLines("ALARM a\nALARM b\nALARM c\n", "ALARM ") == 3);
    CHECK(countPrefixedLines("ALARM none\n", "ALARM ") == 1);       // 占位行也算一行(调用方自行识别)
    CHECK(countPrefixedLines("ALARM a\nRULE type=2 name=HR low=1 high=2 msg=-\n", "ALARM ") == 1);
    CHECK(countPrefixedLines("", "ALARM ") == 0);
    CHECK(countPrefixedLines("ALARM x", "ALARM ") == 1);           // 末行无换行
}

static void test_names() {
    SECTION("kindName / isKnownKind / describe");
    CHECK(kindName(1) == "TEMP");
    CHECK(kindName(2) == "HR");
    CHECK(kindName(3) == "SPO2");
    CHECK(kindName(4) == "CONC");
    CHECK(kindName(5) == "PRESS");
    CHECK(kindName(0) == "UNK" && kindName(200) == "UNK");         // 未知不外泄崩溃
    CHECK(isKnownKind(1) && isKnownKind(5));
    CHECK(!isKnownKind(0) && !isKnownKind(6) && !isKnownKind(255));
    dmp::AlarmRule r; r.type = dmp::TYPE_TEMPERATURE; r.low = 35.f; r.high = 42.f; r.message = "temp";
    const std::string d = describe(r);
    CHECK(d.find("TEMP") != std::string::npos);
    CHECK(d.find("35.000000") != std::string::npos && d.find("42.000000") != std::string::npos);
}

int main() {
    test_parse_rule_line();
    test_float_strict();
    test_level();
    test_stats_and_count();
    test_names();
    std::printf("\nRULE_MAPPING: run=%d fail=%d -> %s\n", g_run, g_fail, g_fail == 0 ? "ALL PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
