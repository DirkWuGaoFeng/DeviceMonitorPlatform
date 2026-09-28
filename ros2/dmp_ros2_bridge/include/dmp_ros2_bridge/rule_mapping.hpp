// dmp_ros2_bridge/rule_mapping.hpp — 桥内的纯函数层 (无 ROS 依赖, 可独立断言)
//
// 为什么单独抽这层: 桥的价值判断都在这几个函数里 (网关文本 <-> 语义结构 <-> 诊断等级)。
// 把它们与 rclcpp 解耦后, 测试不需要起节点、不需要 DDS, 与仓库既有 171 项断言同一套路。
#pragma once

#include "dmp/acquisition.h"     // AlarmRule
#include "dmp/frame_protocol.h"  // SampleType
#include "dmp/service_proto.h"   // typeName (与网关同源的名字表)

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace dmpbr {

// 与 diagnostic_msgs/msg/DiagnosticStatus.level 同序, 发布时可直接 static_cast, 无需映射表
enum class HealthLevel : uint8_t { Ok = 0, Warn = 1, Error = 2 };

// 量纲类型 -> 可打印名 (复用网关表, 避免两处拼写漂移)
inline std::string kindName(uint8_t kind) {
    return dmp::svc::typeName(static_cast<dmp::SampleType>(kind));
}

inline bool isKnownKind(uint8_t kind) {
    switch (kind) {
        case dmp::TYPE_TEMPERATURE: case dmp::TYPE_HEART_RATE: case dmp::TYPE_SPO2:
        case dmp::TYPE_CONCENTR:    case dmp::TYPE_PRESSURE:   return true;
        default: return false;
    }
}

// 严格浮点解析: 整个 token 必须是一个数。strtod 对 "abc" 返回 0.0 且不报错,
// 直接用它会把畸形的配置行当成"低边界为 0"的真实规则 (静默错配比拒收危重得多)。
inline bool parseFloatStrict(const std::string& tok, float* out) {
    if (tok.empty() || !out) return false;
    const char* s = tok.c_str();
    char* end = nullptr;
    errno = 0;
    const double v = std::strtod(s, &end);
    if (end == s || errno != 0) return false;
    while (end < s + tok.size() && (end[0] == '\r' || end[0] == '\n' || end[0] == ' ')) ++end;
    if (end != s + tok.size()) return false;               // 尾巴上还有字符 ("40abc") 一律拒
    if (!std::isfinite(v)) return false;
    *out = static_cast<float>(v);
    return true;
}

// 解析网关 RULES 回读行:
//   "RULE type=2 name=HR low=40.000 high=120.000 msg=hr too fast"
// 返回 false 表示该行不是规则 (调用方据此跳过 "+SUBSCRIBED" 之类杂散行)。
// 注: msg= 总在行尾且可含空格, 因此取"到行尾"而非"到下一个空格"。
inline bool parseRuleLine(const std::string& line, dmp::AlarmRule* out) {
    if (!out) return false;
    if (line.rfind("RULE ", 0) != 0) return false;

    auto field = [&line](const std::string& key) -> std::string {
        const size_t p = line.find(key);
        if (p == std::string::npos) return {};
        const size_t b = p + key.size();
        const size_t e = line.find(' ', b);
        return line.substr(b, e == std::string::npos ? std::string::npos : e - b);
    };

    const std::string t = field("type="), lo = field("low="), hi = field("high=");
    if (t.empty() || lo.empty() || hi.empty()) return false;
    const int type = std::atoi(t.c_str());
    if (!isKnownKind(static_cast<uint8_t>(type))) return false;
    float lowv = 0.f, highv = 0.f;
    if (!parseFloatStrict(lo, &lowv) || !parseFloatStrict(hi, &highv)) return false;
    if (!(lowv < highv)) return false;                     // 词法正确但语义非法(区间倒置/退化)也不接受

    std::string msg;
    const size_t mp = line.find("msg=");
    if (mp != std::string::npos) msg = line.substr(mp + 4);
    while (!msg.empty() && (msg.back() == '\r' || msg.back() == '\n')) msg.pop_back();
    if (msg == "-") msg.clear();                       // 网关对空文案的占位符

    out->type    = static_cast<dmp::SampleType>(type);
    out->low     = lowv;
    out->high    = highv;
    out->message = msg.empty() ? kindName(static_cast<uint8_t>(type)) + " out of range" : msg;
    return true;
}

// 单值 -> 诊断等级。越界即 Error (与 Acquisition 的告警语义一致: 规则不满足就是危急值);
// 未越界但落在边界内侧 warnMarginFrac 的预警带内给 Warn —— 让操作员在越界前就看到趋势。
inline HealthLevel levelFor(float value, const dmp::AlarmRule& r, float warnMarginFrac = 0.05f) {
    if (value < r.low || value > r.high) return HealthLevel::Error;
    const float span = r.high - r.low;
    if (!(span > 0.f) || !(warnMarginFrac > 0.f)) return HealthLevel::Ok;   // 退化区间: 不产生预警带
    const float margin = span * warnMarginFrac;
    if (value < r.low + margin || value > r.high - margin) return HealthLevel::Warn;
    return HealthLevel::Ok;
}

// 规则行文本 (用于服务响应与日志, 与网关 formatRule 同语义但不再依赖其字符串)
inline std::string describe(const dmp::AlarmRule& r) {
    return kindName(static_cast<uint8_t>(r.type)) + " in [" +
           std::to_string(r.low) + ", " + std::to_string(r.high) + "] (" + r.message + ")";
}

// 流级健康计数 (对应网关 "STATS ok=5 crc_err=1 dropped=2")
struct StreamCounters {
    uint64_t ok = 0, crcErr = 0, dropped = 0;
    bool corrupted() const { return crcErr != 0 || dropped != 0; }
};

inline bool parseStatsLine(const std::string& line, StreamCounters* out) {
    if (!out) return false;
    if (line.rfind("STATS ", 0) != 0) return false;
    auto num = [&line](const std::string& key) -> std::pair<bool, uint64_t> {
        const size_t p = line.find(key);
        if (p == std::string::npos) return {false, 0};
        const size_t b = p + key.size();
        size_t e = b;
        while (e < line.size() && std::isdigit(static_cast<unsigned char>(line[e]))) ++e;
        if (e == b) return {false, 0};
        return {true, static_cast<uint64_t>(std::strtoull(line.substr(b, e - b).c_str(), nullptr, 10))};
    };
    const auto ok = num("ok=");
    const auto ce = num("crc_err=");
    const auto dp = num("dropped=");
    if (!ok.first || !ce.first || !dp.first) return false;
    out->ok      = ok.second;
    out->crcErr  = ce.second;
    out->dropped = dp.second;
    return true;
}

// 统计一段应答里以 prefix 开头的行数 (用于 ALARM 计数, 不依赖行尾换行是否存在)
inline size_t countPrefixedLines(const std::string& blob, const std::string& prefix) {
    size_t n = 0, pos = 0;
    while (pos <= blob.size()) {
        const size_t nl = blob.find('\n', pos);
        const std::string line = (nl == std::string::npos)
                                     ? blob.substr(pos)
                                     : blob.substr(pos, nl - pos);
        if (!line.empty() && line.rfind(prefix, 0) == 0) ++n;
        if (nl == std::string::npos) break;
        pos = nl + 1;
    }
    return n;
}

} // namespace dmpbr
