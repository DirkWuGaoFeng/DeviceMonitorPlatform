// dmp_ros2_bridge/bridge_node.hpp — 遥测网关 <-> ROS2 的桥节点 (生命周期管理型)
//
// 职责边界 (刻意保持窄):
//   * 上行: 网关 RAW 帧流 -> /dmp/frames (协议保真层) + /diagnostics (标准诊断视图)
//   * 配置: 网关 RULES/RULE <-> /dmp/rules (锁存状态) + 三个服务
//   * 不做: 任何进入测量/控制链路的写操作; 不做落库; 不做 UI —— 那些是上位机侧的事
//
// 线程模型: 网关读线程只负责解码并写入互斥保护的小缓冲; 执行器线程负责发布与服务应答。
// 这与仓库 pipeline.h 的"生产者 push / 消费者 drain"同构, 因此可直接沿用其有界丢弃语义。
//
// 为什么升级成 LifecycleNode (v0.2 决策, 见 docs/adr/ADR-003):
//   普通 Node 一进场就"要么全跑要么全不跑", 而医疗现场需要三个可分别验证的语义:
//     configure  = 参数与拓扑就绪 + 校验配置本身 (此时不产生任何数据)
//     activate   = 真正开始采集/发布 (数据面与判定链路都只在这里被点着)
//     deactivate = 停止采集但不丢拓扑 (网关侧连接被关闭, 话题仍在但无新数据)
//   于是"误激活/漏激活"变成一个可查询、可断言的状态, 而不是靠看有没有数据猜。
//
// 并发契约 (组合容器里两个回调组会真的并行, 所以这里必须写清):
//   grpData_ : tickFrames / tickDiagnostics      —— 只读 mtx_ 保护的数据 + 只发 frames/diag
//   grpCtl_  : tickLink / tickRules / 三个服务    —— 会做**阻塞**的网关同步 RPC
//   共享状态一律经 mtx_; 网关控制面另由 GatewayClient::ctlMtx_ 串行化。
//   decoder_ 只被网关读线程触碰; device_id_/topic_prefix_ 等在 on_configure 之后只读。
//   控制面一次 RULES 回读要等 ~80ms 静默期且同步阻塞, 所以刻意与数据面分开。
//   但注意: 我原本给的理由 ("单线程容器里这段时间会把同容器**其它设备**的帧发布一起卡住")
//   被自己的 A/B 实验**否了** (2026-09-29): 控制面打满时 bed02 帧最大间隔 212→250ms
//   (自然节律就≈200ms), 批数 60→60 不变。机制解释: 损害被"单个回调时长"(~86ms) 封顶,
//   不随队列总长放大 —— 加深并发窗口 8→32 仍测不出差别。
//   两个回调组因此**保留但降级**: 它是便宜的保险 (一旦真出现单个长回调就会显形),
//   不再是"性能必需"。数字与完整推导见 docs/adr/ADR-003。
#pragma once

#include "dmp/acquisition.h"
#include "dmp_ros2_bridge/gateway_client.hpp"
#include "dmp_ros2_bridge/rule_mapping.hpp"

#include <dmp_msgs/msg/device_frame_array.hpp>
#include <dmp_msgs/msg/rule_list.hpp>
#include <dmp_msgs/srv/get_rules.hpp>
#include <dmp_msgs/srv/selftest.hpp>
#include <dmp_msgs/srv/set_rule.hpp>
#include <diagnostic_msgs/msg/diagnostic_array.hpp>
#include <rclcpp/callback_group.hpp>
#include <rclcpp_lifecycle/lifecycle_node.hpp>
#include <rclcpp_lifecycle/lifecycle_publisher.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dmpbr {

class DmpBridgeNode : public rclcpp_lifecycle::LifecycleNode {
public:
    explicit DmpBridgeNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());
    ~DmpBridgeNode() override;

    // ---- 生命周期回调 (Humble: 返回类型来自 LifecycleNodeInterface 的枚举类) ----
    // 约定: 只有"配置本身不合法"才返回 FAILURE; "对端还没准备好"是 WARN + 重试,
    // 否则网关比桥晚起就会让整个 launch 失败 —— 那会把时序问题伪装成配置错误。
    CallbackReturn on_configure(const rclcpp_lifecycle::State& previous_state) override;
    CallbackReturn on_activate(const rclcpp_lifecycle::State& previous_state) override;
    CallbackReturn on_deactivate(const rclcpp_lifecycle::State& previous_state) override;
    CallbackReturn on_cleanup(const rclcpp_lifecycle::State& previous_state) override;
    CallbackReturn on_shutdown(const rclcpp_lifecycle::State& previous_state) override;

private:
    using DeviceFrameArray = dmp_msgs::msg::DeviceFrameArray;
    using RuleList         = dmp_msgs::msg::RuleList;
    using DiagnosticArray  = diagnostic_msgs::msg::DiagnosticArray;
    using FramesPub  = rclcpp_lifecycle::LifecyclePublisher<DeviceFrameArray>::SharedPtr;
    using DiagPub    = rclcpp_lifecycle::LifecyclePublisher<DiagnosticArray>::SharedPtr;
    using RulesPub   = rclcpp_lifecycle::LifecyclePublisher<RuleList>::SharedPtr;

    // ---- 参数与拓扑 ----
    void declareParameters();
    rclcpp::QoS makeFrameQos(const std::string& mode) const;
    // 两个都是非 const: Humble 的 LifecycleNode::get_current_state() 不是 const 成员,
    // 写成 const 会得 "discards qualifiers" —— 与实现文件保持一致。
    bool isActive();                    // 服务回调的守卫 (见 onSetRule)
    std::string stateLabel();           // "active"/"inactive"/"unconfigured" -> 写进应答里

    // ---- 数据面回调 (网关读线程) ----
    void onBytes(const uint8_t* buf, size_t len);

    // ---- 执行器侧周期任务 ----
    void tickLink();          // 维持数据面连接 (重连 + 退避)
    void tickFrames();        // 批量发布帧
    void tickDiagnostics();   // 汇总通道现值 + 链路健康 -> 标准诊断
    void tickRules();         // 从网关回读配置并锁存发布

    // ---- 服务 ----
    void onSelftest(const std::shared_ptr<dmp_msgs::srv::Selftest::Request> req,
                    std::shared_ptr<dmp_msgs::srv::Selftest::Response> res);
    void onSetRule(const std::shared_ptr<dmp_msgs::srv::SetRule::Request> req,
                   std::shared_ptr<dmp_msgs::srv::SetRule::Response> res);
    void onGetRules(const std::shared_ptr<dmp_msgs::srv::GetRules::Request> req,
                    std::shared_ptr<dmp_msgs::srv::GetRules::Response> res);
    bool refreshRules();      // 控制面 RULES 回读 -> rulesByKind_
    void publishRules();      // rulesByKind_ -> 锁存话题 (自行加锁, 调用者不得持 mtx_)

    // ---- 成员 ----
    GatewayClient      gw_;
    dmp::FrameDecoder  decoder_;                 // 仅读线程访问
    std::string        device_id_, gw_host_, topic_prefix_;
    std::string        qosParam_ = "sensor";      // frame_qos 参数的落地值
    uint16_t           gw_port_ = 9100;
    int                batch_ms_ = 50, diag_ms_ = 1000, rules_ms_ = 10000, link_ms_ = 1000;
    double             warn_margin_ = 0.05;
    size_t             max_pending_ = 4096;
    uint64_t           bridgeDropped_ = 0;       // 本桥缓冲溢出丢弃 (与网关 dropped 分开)

    std::mutex         mtx_;
    std::vector<dmp::Sample> pending_;
    std::map<uint8_t, dmp::Sample> lastByChannel_;
    std::map<uint8_t, dmp::AlarmRule> rulesByKind_;
    StreamCounters     counters_;

    // 回调组: configure 建, cleanup 毁 (激活状态只决定定时器在不在线)
    rclcpp::CallbackGroup::SharedPtr grpData_, grpCtl_;

    FramesPub  pubFrames_;
    DiagPub    pubDiag_;
    RulesPub   pubRules_;
    rclcpp::Service<dmp_msgs::srv::Selftest>::SharedPtr srvSelftest_;
    rclcpp::Service<dmp_msgs::srv::SetRule>::SharedPtr  srvSetRule_;
    rclcpp::Service<dmp_msgs::srv::GetRules>::SharedPtr srvGetRules_;
    std::vector<rclcpp::TimerBase::SharedPtr>        timers_;
};

} // namespace dmpbr
