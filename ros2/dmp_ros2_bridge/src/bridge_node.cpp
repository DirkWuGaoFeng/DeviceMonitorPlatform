// dmp_ros2_bridge/bridge_node.cpp — 桥节点实现
#include "dmp_ros2_bridge/bridge_node.hpp"

#include "dmp/frame_protocol.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <utility>

namespace dmpbr {

namespace {
uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// 把一段多行应答拆成行 (忽略空尾行)
std::vector<std::string> splitLines(const std::string& blob) {
    std::vector<std::string> out;
    std::istringstream ss(blob);
    std::string line;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) out.push_back(line);
    }
    return out;
}

std::string trimTail(const std::string& s) {
    size_t e = s.size();
    while (e > 0 && (s[e - 1] == '\n' || s[e - 1] == '\r')) --e;
    return s.substr(0, e);
}
} // namespace

void DmpBridgeNode::declareParameters() {
    device_id_    = this->declare_parameter<std::string>("device_id", "bed01");
    gw_host_      = this->declare_parameter<std::string>("gateway_host", "127.0.0.1");
    gw_port_      = static_cast<uint16_t>(this->declare_parameter<int>("gateway_port", 9100));
    topic_prefix_ = this->declare_parameter<std::string>("topic_prefix", "/dmp");
    batch_ms_     = this->declare_parameter<int>("batch_period_ms", 50);
    diag_ms_      = this->declare_parameter<int>("diag_period_ms", 1000);
    rules_ms_     = this->declare_parameter<int>("rule_refresh_ms", 10000);
    link_ms_      = this->declare_parameter<int>("link_check_ms", 1000);
    warn_margin_  = this->declare_parameter<double>("warn_margin", 0.05);
    max_pending_  = static_cast<size_t>(this->declare_parameter<int>("max_pending", 4096));
    // 帧流 QoS 可配: 默认 sensor(best_effort)。之所以留成参数, 是为了能"按配置复现"
    // 发布/订阅 QoS 不匹配导致的静默无数据 —— 见 ros2/README.md 的 QoS 事故复盘。
    const std::string qosMode = this->declare_parameter<std::string>("frame_qos", "sensor");

    gw_.setEndpoint(gw_host_, gw_port_);
    RCLCPP_INFO(this->get_logger(), "bridge '%s' -> gateway %s:%u (prefix=%s, frame_qos=%s)",
                device_id_.c_str(), gw_host_.c_str(), gw_port_, topic_prefix_.c_str(), qosMode.c_str());
    qosParam_ = qosMode;
}

rclcpp::QoS DmpBridgeNode::makeFrameQos(const std::string& mode) const {
    rclcpp::QoS qos(rclcpp::KeepLast(20));
    if (mode == "sensor") {
        qos.best_effort();                    // 高频遥测: 宁可丢旧样也不堆积、不阻塞
    } else if (mode == "reliable") {
        qos.reliable();                       // 与 sensor 订阅端错配时会"连得上但收不到", 正是演示点
    } else if (mode == "reliable_deep") {
        qos.reliable();
        qos.get_rmw_qos_profile().depth = 100;
    }
    return qos;
}

// 节点名优先取 NodeOptions (launch 里可按设备起多个实例), 未指定时回退 dmp_bridge。
// 如果不读 options.node_name() 而硬编码名字, launch 传的 name= 会被静默忽略 —— 多实例同构时这是个隐蔽坑。
DmpBridgeNode::DmpBridgeNode(const rclcpp::NodeOptions& options)
    : rclcpp::Node(options.node_name().empty() ? std::string("dmp_bridge") : options.node_name(),
                   options) {
    declareParameters();

    pubFrames_ = this->create_publisher<DeviceFrameArray>(topic_prefix_ + "/frames", makeFrameQos(qosParam_));
    pubDiag_   = this->create_publisher<DiagnosticArray>("/diagnostics",
                    rclcpp::QoS(rclcpp::KeepLast(10)).reliable());
    // 配置是"状态"而非"事件": 锁存 + 持久, 晚起的订阅者也应立刻看到当前阈值
    pubRules_  = this->create_publisher<RuleList>(topic_prefix_ + "/rules",
                    rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local());

    srvSelftest_ = this->create_service<dmp_msgs::srv::Selftest>(
        topic_prefix_ + "/" + device_id_ + "/selftest",
        std::bind(&DmpBridgeNode::onSelftest, this, std::placeholders::_1, std::placeholders::_2));
    srvSetRule_ = this->create_service<dmp_msgs::srv::SetRule>(
        topic_prefix_ + "/" + device_id_ + "/set_rule",
        std::bind(&DmpBridgeNode::onSetRule, this, std::placeholders::_1, std::placeholders::_2));
    srvGetRules_ = this->create_service<dmp_msgs::srv::GetRules>(
        topic_prefix_ + "/" + device_id_ + "/get_rules",
        std::bind(&DmpBridgeNode::onGetRules, this, std::placeholders::_1, std::placeholders::_2));

    using namespace std::chrono_literals;
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(link_ms_),
        [this]() { tickLink(); }));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(batch_ms_),
        [this]() { tickFrames(); }));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(diag_ms_),
        [this]() { tickDiagnostics(); }));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(rules_ms_),
        [this]() { tickRules(); }));

    refreshRules();          // 先拿到阈值, 首轮诊断才有判据
    publishRules();
    tickLink();
}

// ---------------- 数据面 ----------------
void DmpBridgeNode::onBytes(const uint8_t* buf, size_t len) {
    const uint64_t ts = nowMs();
    std::vector<dmp::Sample> got;
    got.reserve(len / dmp::FRAME_LEN + 1);
    decoder_.feed(buf, len, [&](dmp::Sample s) { s.recv_ts = ts; got.push_back(s); });

    const auto st = decoder_.stat();
    std::lock_guard<std::mutex> lk(mtx_);
    counters_.ok      = st.ok;
    counters_.crcErr  = st.crcErr;
    for (const auto& s : got) {
        if (pending_.size() >= max_pending_) { pending_.erase(pending_.begin()); ++bridgeDropped_; }
        pending_.push_back(s);
        lastByChannel_[s.channel] = s;
    }
}

void DmpBridgeNode::tickLink() {
    if (gw_.dataUp()) return;
    if (gw_.ensureData([this](const uint8_t* b, size_t n) { onBytes(b, n); })) {
        RCLCPP_INFO(this->get_logger(), "data plane up (reconnects=%llu)",
                    static_cast<unsigned long long>(gw_.reconnects()));
    } else {
        RCLCPP_WARN(this->get_logger(), "gateway %s:%u 数据面未就绪, %dms 后重试",
                    gw_host_.c_str(), gw_port_, link_ms_);
    }
}

void DmpBridgeNode::tickFrames() {
    std::vector<dmp::Sample> batch;
    StreamCounters c;
    uint64_t bdrop = 0;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (pending_.empty()) return;
        batch.swap(pending_);
        c = counters_;
        bdrop = bridgeDropped_;
    }
    auto msg = std::make_unique<DeviceFrameArray>();
    msg->header.stamp = this->get_clock()->now();
    msg->header.frame_id = device_id_;
    msg->ok      = c.ok;
    msg->crc_err = c.crcErr;
    msg->dropped = bdrop;                       // 注意: 这是本桥丢弃, 网关丢弃见 selftest
    msg->frames.reserve(batch.size());
    for (const auto& s : batch) {
        dmp_msgs::msg::DeviceFrame f;
        f.header.stamp = msg->header.stamp;
        f.header.frame_id = device_id_;
        f.seq       = s.seq;
        f.channel   = s.channel;
        f.kind      = static_cast<uint8_t>(s.type);
        f.kind_name = kindName(static_cast<uint8_t>(s.type));
        f.value     = s.value;
        msg->frames.push_back(std::move(f));
    }
    pubFrames_->publish(std::move(msg));
}

// ---------------- 标准诊断视图 ----------------
void DmpBridgeNode::tickDiagnostics() {
    std::map<uint8_t, dmp::Sample> last;
    std::map<uint8_t, dmp::AlarmRule> rules;
    StreamCounters c;
    uint64_t bdrop = 0;
    bool up = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        last = lastByChannel_;
        rules = rulesByKind_;
        c = counters_;
        bdrop = bridgeDropped_;
        up = gw_.dataUp();
    }

    DiagnosticArray arr;
    arr.header.stamp = this->get_clock()->now();
    auto kv = [](const std::string& k, const std::string& v) {
        diagnostic_msgs::msg::KeyValuePair p;
        p.key = k; p.value = v;
        return p;
    };

    // 1) 链路健康 (先看连接, 再看完整性)
    {
        diagnostic_msgs::msg::DiagnosticStatus st;
        st.name        = "telemetry_link";
        st.hardware_id = device_id_;
        st.values.push_back(kv("data_plane", up ? "up" : "down"));
        st.values.push_back(kv("decoded_ok", std::to_string(c.ok)));
        st.values.push_back(kv("crc_err", std::to_string(c.crcErr)));
        st.values.push_back(kv("bridge_dropped", std::to_string(bdrop)));
        st.values.push_back(kv("reconnects", std::to_string(gw_.reconnects())));
        if (!up) {
            st.level   = diagnostic_msgs::msg::DiagnosticStatus::ERROR;
            st.message = "data plane down";
        } else if (c.corrupted()) {
            st.level   = diagnostic_msgs::msg::DiagnosticStatus::WARN;
            st.message = "stream has crc errors or drops";
        } else {
            st.level   = diagnostic_msgs::msg::DiagnosticStatus::OK;
            st.message = "stream clean";
        }
        arr.status.push_back(st);
    }

    // 2) 每个通道一条状态: 现值 vs 网关下发的阈值
    for (const auto& [ch, s] : last) {
        diagnostic_msgs::msg::DiagnosticStatus st;
        st.name        = kindName(static_cast<uint8_t>(s.type)) + "_ch" + std::to_string(ch);
        st.hardware_id = device_id_;
        st.values.push_back(kv("value", std::to_string(s.value)));
        st.values.push_back(kv("seq", std::to_string(s.seq)));
        st.values.push_back(kv("recv_ts_ms", std::to_string(s.recv_ts)));
        const auto it = rules.find(static_cast<uint8_t>(s.type));
        if (it == rules.end()) {
            st.level   = diagnostic_msgs::msg::DiagnosticStatus::OK;
            st.message = "no rule for kind";
            st.values.push_back(kv("rule", "-"));
        } else {
            const auto lvl = levelFor(s.value, it->second, static_cast<float>(warn_margin_));
            st.level   = static_cast<uint8_t>(lvl);
            st.message = (lvl == HealthLevel::Ok) ? "in range" : describe(it->second);
            st.values.push_back(kv("rule", describe(it->second)));
        }
        arr.status.push_back(st);
    }
    pubDiag_->publish(arr);
}

// ---------------- 配置面 ----------------
bool DmpBridgeNode::refreshRules() {
    const std::string blob = gw_.requestLines("RULES", "RULE ");
    if (blob.empty()) return false;
    std::map<uint8_t, dmp::AlarmRule> parsed;
    for (const auto& line : splitLines(blob)) {
        dmp::AlarmRule r;
        if (parseRuleLine(line, &r)) parsed[static_cast<uint8_t>(r.type)] = r;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    rulesByKind_ = std::move(parsed);
    return true;
}

void DmpBridgeNode::publishRules() {
    RuleList msg;
    msg.header.stamp = this->get_clock()->now();
    msg.header.frame_id = device_id_;
    msg.source = "gateway:" + gw_host_ + ":" + std::to_string(gw_port_);
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (const auto& [kind, r] : rulesByKind_) {
            dmp_msgs::msg::Rule m;
            m.kind = kind;
            m.kind_name = kindName(kind);
            m.low = r.low;
            m.high = r.high;
            m.message = r.message;
            msg.rules.push_back(std::move(m));
        }
    }
    pubRules_->publish(msg);
}

void DmpBridgeNode::tickRules() {
    if (refreshRules()) publishRules();
    else RCLCPP_WARN(this->get_logger(), "RULES 回读失败 (控制面不可用?)");
}

void DmpBridgeNode::onGetRules(const std::shared_ptr<dmp_msgs::srv::GetRules::Request>,
                               std::shared_ptr<dmp_msgs::srv::GetRules::Response> res) {
    refreshRules();
    {
        std::lock_guard<std::mutex> lk(mtx_);        // 仅在拷贝期间持锁
        for (const auto& [kind, r] : rulesByKind_) {
            dmp_msgs::msg::Rule m;
            m.kind = kind;
            m.kind_name = kindName(kind);
            m.low = r.low;
            m.high = r.high;
            m.message = r.message;
            res->rules.push_back(std::move(m));
        }
    }                        // 锁必须在此释放: publishRules 内部会再次加锁,
    publishRules();          // 而 std::mutex 不可重入, 嵌套加锁会死锁
}

void DmpBridgeNode::onSelftest(const std::shared_ptr<dmp_msgs::srv::Selftest::Request> req,
                               std::shared_ptr<dmp_msgs::srv::Selftest::Response> res) {
    std::ostringstream detail;
    const std::string stats = gw_.requestOne("STATS");
    StreamCounters gc;
    const bool statsOk = parseStatsLine(stats, &gc);
    detail << "stats=" << trimTail(stats.empty() ? "STATS unavailable" : stats) << '\n';

    const std::string alarms = gw_.requestLines("ALARMS 5", "ALARM ");
    res->recent_alarms = static_cast<uint32_t>(countPrefixedLines(alarms, "ALARM "));
    detail << "recent_alarm_lines=" << res->recent_alarms << '\n';

    res->ok       = gc.ok;
    res->crc_err  = gc.crcErr;
    res->dropped  = gc.dropped;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        res->rule_count = static_cast<uint32_t>(rulesByKind_.size());
    }
    res->healthy = gw_.dataUp() && statsOk && !gc.corrupted();
    detail << "data_plane=" << (gw_.dataUp() ? "up" : "down")
           << " probe=" << (req->probe.empty() ? "-" : req->probe);
    res->detail = detail.str();
}

void DmpBridgeNode::onSetRule(const std::shared_ptr<dmp_msgs::srv::SetRule::Request> req,
                              std::shared_ptr<dmp_msgs::srv::SetRule::Response> res) {
    // 边界: 只允许写告警判定阈值; kind 未知或区间非法一律本地拒绝, 不转发给网关。
    if (!isKnownKind(req->kind)) {
        res->accepted = false;
        res->reason = "unknown kind " + std::to_string(req->kind) + " (allowed 1..5)";
        RCLCPP_WARN(this->get_logger(), "set_rule refused: %s", res->reason.c_str());
        return;
    }
    if (!(req->low < req->high)) {
        res->accepted = false;
        res->reason = "need low < high";
        return;
    }
    std::ostringstream cmd;
    cmd << "RULE " << static_cast<int>(req->kind) << ' ' << req->low << ' ' << req->high;
    if (!req->message.empty()) cmd << ' ' << req->message;
    const std::string resp = gw_.requestOne(cmd.str());
    if (resp.rfind("RULE_ACK", 0) != 0) {
        res->accepted = false;
        res->reason = trimTail(resp.empty() ? "gateway unavailable" : resp);
        RCLCPP_ERROR(this->get_logger(), "set_rule failed: %s", res->reason.c_str());
        return;
    }
    // 回读网关实际生效值 (而非调用方声称值), 保证 ROS 侧与网关侧配置单一来源
    refreshRules();
    dmp::AlarmRule applied;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        auto it = rulesByKind_.find(req->kind);
        if (it != rulesByKind_.end()) { applied = it->second; found = true; }
    }
    res->accepted = true;
    res->reason   = "applied";
    if (found) {
        res->applied.kind = req->kind;
        res->applied.kind_name = kindName(req->kind);
        res->applied.low = applied.low;
        res->applied.high = applied.high;
        res->applied.message = applied.message;
    }
    publishRules();
    RCLCPP_INFO(this->get_logger(), "set_rule applied: %s",
                found ? describe(applied).c_str() : "(ack without readback)");
}

} // namespace dmpbr
