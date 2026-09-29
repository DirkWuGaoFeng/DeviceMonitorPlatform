// dmp/service_proto.h — 遥测网关的文本行协议 (纯函数, 零 socket 依赖, 可单测)
//
// 设计意图: 把"采集/告警后端"抽象成一个可远程访问的服务。本机无 gRPC 工具链,
//   故先用一行一命令的文本协议落地一个真实可跑的 TCP 网关 (gateway_service.cpp);
//   proto/telemetry.proto 是同语义的 gRPC 契约, 工具链就绪即可平替传输层而不动业务。
//
// 协议 (客户端 -> 服务端, 一行一命令; 服务端 -> 客户端, 一行一应答/推送):
//   HELP                        -> COMMANDS subscribe stats alarms help
//   STATS                       -> STATS ok=.. crc_err=.. dropped=..
//   ALARMS [limit]              -> 逐条 ALARM .. (最多 limit 条, 默认 10)
//   SUBSCRIBE                   -> 进入推送模式, 后续每条样本一行 SAMPLE ..
//   RAW                         -> 订阅原始帧字节流(上游逐字节透传, 二进制); 供下游再解码(gRPC 服务)
//   HISTORY [ch] [windowSec]    -> 近 windowSec 秒的逐通道聚合: HISTORY ch=.. count=.. avg=.. min=.. max=.. last=..
//                                  (ch 缺省/负数 = 全部有数据通道; 供 LLM Agent 做告警归因)
//   RULES                       -> 逐条 RULE type=.. name=.. low=.. high=.. msg=..   (配置回读)
//   RULE <type> <low> <high> [msg] -> RULE_ACK accepted=1 ..  幂等下发同类型覆盖
//                                  (type 可数字 1..5 或名字 TEMP/HR/SPO2/CONC/PRESS)
//   CLIENTS                     -> 逐条 CLIENT id=.. sub=.. raw=.. qbytes=.. ..  (每连接出向队列体检)
//   未知                        -> ERR unknown command: X
// 所有应答以 '\n' 结尾, 便于客户端按行读取。
#pragma once

#include "dmp/acquisition.h"   // Sample, AlarmEvent
#include "dmp/frame_protocol.h"

#include <cctype>
#include <cstdio>
#include <sstream>
#include <string>

namespace dmp {
namespace svc {

enum class CmdKind { Help, Stats, Alarms, Subscribe, Raw, History, Rules, SetRule, Clients, Unknown };

struct Command {
    CmdKind kind = CmdKind::Unknown;
    int     arg  = 0;        // ALARMS [limit] / HISTORY [ch] / RULE [type]
    int     arg2 = 0;        // HISTORY [windowSec]
    float   f1   = 0.f;      // RULE low
    float   f2   = 0.f;      // RULE high
    std::string name;        // 原始命令词 (小写), 未知命令回显用
    std::string text;        // RULE 的剩余部分 = 告警文案 (可含空格)
};

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

// 名字 -> 类型编号 (大小写不敏感; 也接受字面数字 1..5); 非法返回 0
// 注: 本函数必须定义在 parseCommand 之前 —— 后者对它的调用是非依赖名, 需先可见
inline int typeFromWord(const std::string& in) {
    std::string s = in;
    for (char& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (s == "TEMP" || s == "TEMPERATURE") return TYPE_TEMPERATURE;
    if (s == "HR"   || s == "HEART_RATE")  return TYPE_HEART_RATE;
    if (s == "SPO2")                       return TYPE_SPO2;
    if (s == "CONC" || s == "CONCENTR")    return TYPE_CONCENTR;
    if (s == "PRESS"|| s == "PRESSURE")    return TYPE_PRESSURE;
    if (s.size() == 1 && s[0] >= '1' && s[0] <= '5') return s[0] - '0';
    return 0;
}

// 解析一行命令 (大小写不敏感); 去掉行尾 \r\n
inline Command parseCommand(const std::string& line) {
    Command c;
    std::istringstream ss(trim(line));
    std::string word;
    if (!(ss >> word)) { c.kind = CmdKind::Help; return c; }   // 空行当 help
    for (char& ch : word) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    c.name = word;
    if (word == "help")   c.kind = CmdKind::Help;
    else if (word == "stats")    c.kind = CmdKind::Stats;
    else if (word == "alarms")   { c.kind = CmdKind::Alarms; c.arg = 10; ss >> c.arg; if (c.arg <= 0) c.arg = 10; }
    else if (word == "subscribe") c.kind = CmdKind::Subscribe;
    else if (word == "raw")       c.kind = CmdKind::Raw;
    else if (word == "history")   { c.kind = CmdKind::History; c.arg = -1; c.arg2 = 60;
                                    ss >> c.arg; if (c.arg < -1) c.arg = -1;
                                    ss >> c.arg2; if (c.arg2 <= 0) c.arg2 = 60; }
    else if (word == "rules")     c.kind = CmdKind::Rules;
    else if (word == "clients")    c.kind = CmdKind::Clients;
    else if (word == "rule") {
        c.kind = CmdKind::SetRule;
        std::string tv;
        if (!(ss >> tv)) { c.kind = CmdKind::Unknown; c.name = word + " <missing type>"; return c; }
        int t = typeFromWord(tv);
        if (t <= 0) { c.kind = CmdKind::Unknown; c.name = word + " <bad type: " + tv + ">"; return c; }
        c.arg = t;
        if (!(ss >> c.f1) || !(ss >> c.f2)) { c.kind = CmdKind::Unknown; c.name = word + " <need low high>"; return c; }
        std::getline(ss, c.text);            // 余下全定为告警文案
        c.text = trim(c.text);
    }
    else c.kind = CmdKind::Unknown;
    return c;
}

inline std::string formatStats(uint64_t ok, uint64_t crcErr, uint64_t dropped) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "STATS ok=%llu crc_err=%llu dropped=%llu\n",
                  static_cast<unsigned long long>(ok),
                  static_cast<unsigned long long>(crcErr),
                  static_cast<unsigned long long>(dropped));
    return buf;
}

inline std::string typeName(SampleType t) {
    switch (t) {
        case TYPE_TEMPERATURE: return "TEMP";
        case TYPE_HEART_RATE:  return "HR";
        case TYPE_SPO2:        return "SPO2";
        case TYPE_CONCENTR:    return "CONC";
        case TYPE_PRESSURE:    return "PRESS";
        default:               return "UNK";
    }
}

inline std::string formatSample(const Sample& s) {
    char buf[160];
    std::snprintf(buf, sizeof(buf), "SAMPLE ts=%llu seq=%u ch=%u type=%u name=%s value=%.3f\n",
                  static_cast<unsigned long long>(s.recv_ts), s.seq, s.channel,
                  static_cast<unsigned>(s.type), typeName(s.type).c_str(), s.value);
    return buf;
}

inline std::string formatAlarm(const AlarmEvent& a) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "ALARM ts=%llu ch=%u type=%u name=%s value=%.3f critical=%d msg=%s\n",
                  static_cast<unsigned long long>(a.sample.recv_ts), a.sample.channel,
                  static_cast<unsigned>(a.sample.type), typeName(a.sample.type).c_str(), a.sample.value,
                  a.critical ? 1 : 0, a.message.c_str());
    return buf;
}

inline std::string formatError(const std::string& msg) { return "ERR " + msg + "\n"; }
inline std::string formatHelp() {
    return "COMMANDS subscribe raw history[ch][sec] stats alarms[limit] rules rule[type][low][high][msg] clients help\n";
}

// 每连接出向队列的自述行 (T1.1 / SR-014)。
// 为何要单独开一条命令而不往 STATS 里加字段：STATS 的行格式已被下游脚本与 Agent 正则
// 依赖（见 tools/dmp_agent.py 的 stats()），改格式是**破接口**；新增命令只新增能力。
// 三档丢弃分列三列 (drop 为总、evict 为其子集、kick 单独计)：只给一个总数的话，
// “样本被挤掉”与“应答腾不出位”这两种完全不一样的故障会被读成同一件事。
struct ClientStat {
    uint64_t id = 0;
    int      sub = 0, raw = 0;
    uint64_t qbytes = 0, lines = 0, stalled = 0;
    uint64_t enqLines = 0, sentBytes = 0, dropLines = 0, evictLines = 0, kickLines = 0;
};
inline std::string formatClient(const ClientStat& s) {
    // 缓冲尺寸按**最坏情形**定：9 个字段全取 uint64 满值时实测长 257 字节，
    // 取 256 会把最后的 kick= 截掉——下游正则读到一个被截短的数字比读不到更难查。
    char buf[320];
    std::snprintf(buf, sizeof(buf),
                  "CLIENT id=%llu sub=%d raw=%d qbytes=%llu lines=%llu stalled=%llu "
                  "enq=%llu sent=%llu drop=%llu evict=%llu kick=%llu\n",
                  static_cast<unsigned long long>(s.id), s.sub, s.raw,
                  static_cast<unsigned long long>(s.qbytes),
                  static_cast<unsigned long long>(s.lines),
                  static_cast<unsigned long long>(s.stalled),
                  static_cast<unsigned long long>(s.enqLines),
                  static_cast<unsigned long long>(s.sentBytes),
                  static_cast<unsigned long long>(s.dropLines),
                  static_cast<unsigned long long>(s.evictLines),
                  static_cast<unsigned long long>(s.kickLines));
    return buf;
}

// 配置回读行: 与 AlarmRule 逐字段对应, 便于下游无歧义解析
inline std::string formatRule(const AlarmRule& r) {
    char buf[256];
    std::snprintf(buf, sizeof(buf), "RULE type=%u name=%s low=%.3f high=%.3f msg=%s\n",
                  static_cast<unsigned>(r.type), typeName(r.type).c_str(),
                  static_cast<double>(r.low), static_cast<double>(r.high),
                  r.message.empty() ? "-" : r.message.c_str());
    return buf;
}

// 下发回执: 回显生效后的实际边界 (而非客户端声称的值), 避免精度/截断造成双向不一致
inline std::string formatRuleAck(const AlarmRule& applied) {
    return "RULE_ACK accepted=1 " + formatRule(applied);
}

// HISTORY 聚合行 (由调用方汇总窗口内样本后生成; 一行一通道)
struct HistAgg {
    int      channel = 0;
    uint64_t count = 0;
    double   avg = 0, min = 0, max = 0, last = 0;
};
inline std::string formatHistory(const HistAgg& a) {
    char buf[160];
    std::snprintf(buf, sizeof(buf),
                  "HISTORY ch=%u count=%llu avg=%.3f min=%.3f max=%.3f last=%.3f\n",
                  a.channel, static_cast<unsigned long long>(a.count),
                  a.avg, a.min, a.max, a.last);
    return buf;
}

} // namespace svc
} // namespace dmp
