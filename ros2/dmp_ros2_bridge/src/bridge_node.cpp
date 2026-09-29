// dmp_ros2_bridge/bridge_node.cpp — 桥节点实现 (生命周期管理型)
#include "dmp_ros2_bridge/bridge_node.hpp"

#include "dmp/frame_protocol.h"

#include <chrono>
#include <cstdio>
#include <sstream>
#include <utility>

namespace dmpbr {

// 为什么这里必须显式给一个别名 (真编译错: "'CallbackReturn' does not name a type"):
// 头文件里写在类作用域内, 裸名 CallbackReturn 可以沿基类 LifecycleNodeInterface 查到;
// 但 .cpp 里的**类外定义**中, 返回类型出现在 DmpBridgeNode:: 之前, 那时编译器还在
// 名字空间作用域查 —— 基类里的嵌套类型看不见。上游示例也都这么写一行 using。
using CallbackReturn = rclcpp_lifecycle::node_interfaces::LifecycleNodeInterface::CallbackReturn;

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
    // connect 超时: 把 R-003"任何单个回调必须短"从约定变成实现。
    // 0 是**故意合法**的值(= 不设限, 旧行为), 所以它不能跟上面那些周期一起进"必须为正"的校验。
    conn_timeout_ms_ = this->declare_parameter<int>("connect_timeout_ms", 1000);
    warn_margin_  = this->declare_parameter<double>("warn_margin", 0.05);
    max_pending_  = static_cast<size_t>(this->declare_parameter<int>("max_pending", 4096));
    // 帧流 QoS 可配: 默认 sensor(best_effort)。留成参数不是为了演示好看, 而是因为
    // QoS 错配是 ROS2 里唯一"发现得到、不报错、就是没数据"的故障类 —— 必须能被复现才能被写进复盘。
    // 实测方向见 ros2/README.md: 发布 best_effort + 订阅 reliable 才会静默失效。
    const std::string qosMode = this->declare_parameter<std::string>("frame_qos", "sensor");

    gw_.setEndpoint(gw_host_, gw_port_);
    gw_.setConnectTimeoutMs(conn_timeout_ms_);
    qosParam_ = qosMode;
}

// 配置校验: 这里只拒"配置本身不合法", 不拒"对端还没起来"。
// 后者交给 tickLink 重试 —— 如果把时序问题返回成 FAILURE, 整个 launch 会因网关晚启动而失败,
// 于是运维看到的是"配置错误", 而真实原因是"谁先谁后"。这类伪装出来的因果关系最难查。
CallbackReturn DmpBridgeNode::on_configure(const rclcpp_lifecycle::State& /*prev*/) {
    declareParameters();

    if (batch_ms_ <= 0 || diag_ms_ <= 0 || rules_ms_ <= 0 || link_ms_ <= 0) {
        RCLCPP_ERROR(this->get_logger(), "configure 拒绝: 周期必须为正 (batch=%d diag=%d rules=%d link=%d)",
                     batch_ms_, diag_ms_, rules_ms_, link_ms_);
        return CallbackReturn::FAILURE;
    }
    if (gw_port_ == 0) {
        RCLCPP_ERROR(this->get_logger(), "configure 拒绝: gateway_port=0 不是合法端口");
        return CallbackReturn::FAILURE;
    }
    if (conn_timeout_ms_ < 0) {
        // 负值不能静默退化成 0(=不可控长等), 也不能退化成默认值(=口头上的上限);
        // 未知值与静默退化是同一类故障 (参照 frame_qos 的处理)。
        RCLCPP_ERROR(this->get_logger(), "configure 拒绝: connect_timeout_ms=%d 非法 (0=不设限是合法值, 但不允许负)",
                     conn_timeout_ms_);
        return CallbackReturn::FAILURE;
    }
    if (qosParam_ != "sensor" && qosParam_ != "reliable" && qosParam_ != "reliable_deep") {
        // 未知 QoS 名如果静默退化成 sensor, 订阅端就会掉进"零数据且不报错"那一类故障 —— 直接拒。
        RCLCPP_ERROR(this->get_logger(), "configure 拒绝: frame_qos='%s' 未知 (sensor|reliable|reliable_deep)",
                     qosParam_.c_str());
        return CallbackReturn::FAILURE;
    }

    RCLCPP_INFO(this->get_logger(), "bridge '%s' -> gateway %s:%u (prefix=%s, frame_qos=%s)",
                device_id_.c_str(), gw_host_.c_str(), gw_port_, topic_prefix_.c_str(), qosParam_.c_str());

    // 两个回调组: 组合容器里它们会分到不同线程, 于是控制面的阻塞 RPC 不拖累数据面。
    grpData_ = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);
    grpCtl_  = this->create_callback_group(rclcpp::CallbackGroupType::MutuallyExclusive);

    pubFrames_ = this->create_publisher<DeviceFrameArray>(topic_prefix_ + "/frames",
                                                           makeFrameQos(qosParam_));
    pubDiag_   = this->create_publisher<DiagnosticArray>("/diagnostics",
                    rclcpp::QoS(rclcpp::KeepLast(10)).reliable());
    // 配置是"状态"而非"事件": 锁存 + 持久, 晚起的订阅者也应立刻看到当前阈值
    pubRules_  = this->create_publisher<RuleList>(topic_prefix_ + "/rules",
                    rclcpp::QoS(rclcpp::KeepLast(1)).reliable().transient_local());

    // 服务在 configure 就创建 -> inactive 期间**仍然可被调用**, 应答里带当前状态。
    // 刻意不在 activate 才建: 未激活时服务消失, 调用方只会停在 "waiting for service to become
    // available..." 且没有任何错误信息 (这个坑记在素材录 B-15)。可查询的"我没在干活"
    // 比不可查询的沉默安全得多 —— 尤其在医疗语境下。
    const auto svcQos = rmw_qos_profile_services_default;
    srvSelftest_ = this->create_service<dmp_msgs::srv::Selftest>(
        topic_prefix_ + "/" + device_id_ + "/selftest",
        std::bind(&DmpBridgeNode::onSelftest, this, std::placeholders::_1, std::placeholders::_2),
        svcQos, grpCtl_);
    srvSetRule_ = this->create_service<dmp_msgs::srv::SetRule>(
        topic_prefix_ + "/" + device_id_ + "/set_rule",
        std::bind(&DmpBridgeNode::onSetRule, this, std::placeholders::_1, std::placeholders::_2),
        svcQos, grpCtl_);
    srvGetRules_ = this->create_service<dmp_msgs::srv::GetRules>(
        topic_prefix_ + "/" + device_id_ + "/get_rules",
        std::bind(&DmpBridgeNode::onGetRules, this, std::placeholders::_1, std::placeholders::_2),
        svcQos, grpCtl_);

    // configure 阶段只做一次"能不能读到阈值"的尝试: 读到就存着 (不发!), 读不到只 WARN。
    // 为什么不在这里发: LifecyclePublisher 未激活时 publish() 直接 return, 等于发到一个
    // 关着的门上。它不是完全无声 —— should_log_ 初值为 true, 第一次丢弃会打一条 WARN
    // 然后置 false (见 on_activate 上方注释), 所以日志里只有一句, 且 logger 名是
    // "LifecyclePublisher" 而不是节点名, 扫日志时极易看漏。
    if (refreshRules()) {
        RCLCPP_INFO(this->get_logger(), "configure: 读到 %zu 条阈值 (激活后才发布)",
                    rulesByKind_.size());
    } else {
        RCLCPP_WARN(this->get_logger(),
                    "configure: 网关控制面暂不可读 RULES, 激活后由 tickRules 重试 (不算配置错误)");
    }
    return CallbackReturn::SUCCESS;
}

rclcpp::QoS DmpBridgeNode::makeFrameQos(const std::string& mode) const {
    rclcpp::QoS qos(rclcpp::KeepLast(20));
    if (mode == "sensor") {
        qos.best_effort();                    // 高频遥测: 宁可丢旧样也不堆积、不阻塞
    } else if (mode == "reliable") {
        // 注意方向: 发布 RELIABLE + 订阅 best_effort 是兼容的 (实测能收到, 只是不重传)。
        // 真正"静默无数据"的是反方向 —— 发布 best_effort 而订阅端要求 reliable。
        // 四种组合的实测矩阵见 tools/vm_qos_mismatch.sh 与 ros2/README.md。
        qos.reliable();
    } else if (mode == "reliable_deep") {
        qos = rclcpp::QoS(rclcpp::KeepLast(200));   // 深队列: 允许短时下游停顿而不丢帧
        qos.reliable();
        // 注: 不要写 qos.get_rmw_qos_profile().depth = N —— 它返回的是副本, 改完即丢, 是静默失效的写法。
        // 队列深度只能通过 KeepLast(n) 设定。
    }
    return qos;
}

// 两个都得是**非 const**: Humble 的 LifecycleNode::get_current_state() 本身不是 const 成员
// (后来版本才改 const), 所以"只读一个状态"在这里也得跟基类一起不带 const。
// 靠记忆写会编译不过 —— 这是本轮在 VM 上实编得到的事实。
bool DmpBridgeNode::isActive() {
    return get_current_state().label() == "active";
}

std::string DmpBridgeNode::stateLabel() {
    return get_current_state().label();
}

// 节点名写字面量 "dmp_bridge" 即可: launch_ros 的 name= 会下发 __node 重映射规则,
// rcl_init 阶段用它覆盖构造器里的名字, 因此一台设备一个实例的写法成立 (多实例不重名)。
// 之前我写的 options.node_name() 在 Humble 的 NodeOptions 上不存在, 编译即报错 —— 记在此处防复发。
//
// 升级成 LifecycleNode 后构造器**只剩基类初始化**: 所有副作用都挪进生命周期回调。
// 这条线是有意义的 —— "构造一个对象"不应该等于"开始采集病人体征"。
DmpBridgeNode::DmpBridgeNode(const rclcpp::NodeOptions& options)
    : rclcpp_lifecycle::LifecycleNode("dmp_bridge", options) {}

DmpBridgeNode::~DmpBridgeNode() {
    // 兜底: 容器可能在 active 状态下直接被销毁 (没走 deactivate/shutdown 转换)。
    // 网关读线程必须在这里停掉, 否则它会拿着已经析构的 decoder_ 继续 feed —— Use-After-Free。
    gw_.closeAll();
}

CallbackReturn DmpBridgeNode::on_activate(const rclcpp_lifecycle::State& prev) {
    // ★ 必须链回基类, 否则整个数据面永远不会通。位置在两处不一样:
    // activate 里放在**最前** (先开门再发东西), deactivate 里放在**最后** (先停生产者再关门)。
    // Humble 的 LifecycleNode 在基类里只**实现**了 on_activate / on_deactivate 这两个 override
    // (其余生命周期回调留给用户), 它们的职责是遍历 add_managed_entity() 注册的托管实体
    // (create_publisher 会把每个 LifecyclePublisher 注册进去) 并翻 is_activated 开关。
    // 自己 override 而不链回去 => 状态机确实到了 active、话题在、服务在、定时器在跑,
    // 但 publish() 永远被丢弃 —— 上一轮就是这个坑。
    // 链回去而不是手写三个 pubFrames_->on_activate(): 以后多一个 publisher 就不会又忘。
    CallbackReturn ret = LifecycleNode::on_activate(prev);   // 先开门(激活 publisher), 再发东西

    // 定时器只在 active 期存在: deactivate 清空后, 数据面/控制面自然彻底停摆,
    // 不需要在每个 tick 里再判断"我现在激活吗"。
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(link_ms_),
        [this]() { tickLink(); }, grpCtl_));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(batch_ms_),
        [this]() { tickFrames(); }, grpData_));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(diag_ms_),
        [this]() { tickDiagnostics(); }, grpData_));
    timers_.push_back(this->create_wall_timer(std::chrono::milliseconds(rules_ms_),
        [this]() { tickRules(); }, grpCtl_));

    // 先立刻发一帧阈值 + 主动连一次数据面, 省掉一个周期的空窗 (activate 之后
    // 第一个 tick 可能要等 rules_ms_=10s, 验收脚本会误判成"没数据")。
    // 注: 这两句能真的发出去, 前提是上面已经链过基类 —— 本轮先写了这两句、后链基类,
    // 日志里就留下一条"/dmp/rules ... not activated"的 WARN, 因为基类还没开门。
    publishRules();
    tickLink();

    RCLCPP_INFO(this->get_logger(), "activated: 数据面与判定链路已启动 (state=%s)",
                stateLabel().c_str());
    return ret;
}

CallbackReturn DmpBridgeNode::on_deactivate(const rclcpp_lifecycle::State& prev) {
    // 顺序重要: 先停定时器 (不再有人碰 publisher), 再关网关 (join 读线程)。
    // 反过来会留一个窗口: 读线程还在喂数, 而 publisher 已无人驱动 —— 那个窗口不致命,
    // 但会让 pending_ 在下一次 activate 时把上一批"陈旧样本"当新数据发出去。
    timers_.clear();
    const uint64_t okSnap = counters_.ok;
    gw_.closeAll();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.clear();               // 跨激活周期不留旧数据 (见上面注释)
        lastByChannel_.clear();
    }
    // 基类的 on_deactivate 负责把所有托管实体关门。不链回去的话, deactivate 之后
    // is_activated 仍为 true, "inactive 期零数据"就完全靠"定时器正好不在了"这个巧合撑着 ——
    // 一个只靠定时器而不靠门禁的守卫, 在有人新增一条不走定时器的发布路径时就会漏。关闸放在
    // 最后是因为: 先停生产者再关门, 反过来会刷一屏 "publisher is not activated" 的 WARN。
    CallbackReturn ret = LifecycleNode::on_deactivate(prev);
    RCLCPP_INFO(this->get_logger(),
                "deactivated: 已停定时器、关网关连接并关闭发布门, 丢弃未发布样本 (激活期累计解码 ok=%llu)",
                static_cast<unsigned long long>(okSnap));
    return ret;
}

CallbackReturn DmpBridgeNode::on_cleanup(const rclcpp_lifecycle::State& /*prev*/) {
    // 拓扑彻底拆掉: 回到 unconfigured 之后话题/服务都不存在, 与"没起这台设备"一致。
    timers_.clear();
    srvSelftest_.reset(); srvSetRule_.reset(); srvGetRules_.reset();
    pubFrames_.reset(); pubDiag_.reset(); pubRules_.reset();
    grpData_.reset(); grpCtl_.reset();
    gw_.closeAll();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pending_.clear();
        lastByChannel_.clear();
        rulesByKind_.clear();
        counters_ = StreamCounters{};
        bridgeDropped_ = 0;
    }
    decoder_ = dmp::FrameDecoder{};      // 半帧缓冲属于一次配置会话, 不带进下一次
    RCLCPP_INFO(this->get_logger(), "cleaned up");
    return CallbackReturn::SUCCESS;
}

CallbackReturn DmpBridgeNode::on_shutdown(const rclcpp_lifecycle::State& /*prev*/) {
    timers_.clear();
    gw_.closeAll();
    RCLCPP_INFO(this->get_logger(), "shutdown: 桥已静默 (state=%s)", stateLabel().c_str());
    return CallbackReturn::SUCCESS;
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
        // 把 connect 超时次数带进这句 WARN: 毒化实验里它是"长回调已被消除"的直接证据 ——
        // 只从帧间隔倒推的话, 那个结论无法独立复核。
        RCLCPP_WARN(this->get_logger(), "gateway %s:%u 数据面未就绪 (%dms 后重试), connect 超时累计 %llu 次",
                    gw_host_.c_str(), gw_port_, link_ms_,
                    static_cast<unsigned long long>(gw_.connectTimeouts()));
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
        diagnostic_msgs::msg::KeyValue p;   // 类型名是 KeyValue, 不是 KeyValuePair
        p.key = k; p.value = v;
        return p;
    };

    // 1) 链路健康 (先看连接, 再看完整性)
    {
        diagnostic_msgs::msg::DiagnosticStatus st;
        st.name        = "telemetry_link";
        st.hardware_id = device_id_;
        st.values.push_back(kv("state", stateLabel()));       // 生命周期状态本身也是诊断项
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
    // 读操作不加状态守卫: "现在阈值是什么"在未激活时也应当能问到 (拿到的是缓存/空)。
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
    const std::string state = stateLabel();
    const std::string stats = gw_.requestOne("STATS");
    StreamCounters gc;
    const bool statsOk = parseStatsLine(stats, &gc);
    detail << "state=" << state << '\n';
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
    // 未激活时 healthy 必为 false: 自检问的是"这条链路现在活着吗", 不是"配置对不对"。
    res->healthy = (state == "active") && gw_.dataUp() && statsOk && !gc.corrupted();
    detail << "data_plane=" << (gw_.dataUp() ? "up" : "down")
           << " probe=" << (req->probe.empty() ? "-" : req->probe);
    res->detail = detail.str();
}

void DmpBridgeNode::onSetRule(const std::shared_ptr<dmp_msgs::srv::SetRule::Request> req,
                              std::shared_ptr<dmp_msgs::srv::SetRule::Response> res) {
    // 边界 0 (生命周期): 非 active 一律拒写。未激活的桥去改临床告警阈值, 后果是"改了但没人判定",
    // 这比直接报错危险得多 —— 所以这里必须硬拒, 而不是"先存着等激活再下发"。
    if (!isActive()) {
        res->accepted = false;
        res->reason = "bridge not active (state=" + stateLabel() + ")";
        RCLCPP_WARN(this->get_logger(), "set_rule refused: %s", res->reason.c_str());
        return;
    }
    // 边界 1: 只允许写告警判定阈值; kind 未知或区间非法一律本地拒绝, 不转发给网关。
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
