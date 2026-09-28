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

enum class CmdKind { Help, Stats, Alarms, Subscribe, Raw, History, Unknown };

struct Command {
    CmdKind kind = CmdKind::Unknown;
    int     arg  = 0;        // ALARMS [limit] / HISTORY [ch]
    int     arg2 = 0;        // HISTORY [windowSec]
    std::string name;        // 原始命令词 (小写), 未知命令回显用
};

inline std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
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
inline std::string formatHelp() { return "COMMANDS subscribe raw history[ch][sec] stats alarms[limit] help\n"; }

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
