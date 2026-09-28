// dmp_ros2_bridge/bridge_node.hpp — 遥测网关 <-> ROS2 的桥节点
//
// 职责边界 (刻意保持窄):
//   * 上行: 网关 RAW 帧流 -> /dmp/frames (协议保真层) + /diagnostics (标准诊断视图)
//   * 配置: 网关 RULES/RULE <-> /dmp/rules (锁存状态) + 三个服务
//   * 不做: 任何进入测量/控制链路的写操作; 不做落库; 不做 UI —— 那些是上位机侧的事
//
// 线程模型: 网关读线程只负责解码并写入互斥保护的小缓冲; 执行器线程负责发布与服务应答。
// 这与仓库 pipeline.h 的"生产者 push / 消费者 drain"同构, 因此可直接沿用其有界丢弃语义。
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
#include <rclcpp/rclcpp.hpp>

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace dmpbr {

class DmpBridgeNode : public rclcpp::Node {
public:
    explicit DmpBridgeNode(const rclcpp::NodeOptions& options = rclcpp::NodeOptions());

private:
    using DeviceFrameArray = dmp_msgs::msg::DeviceFrameArray;
    using RuleList         = dmp_msgs::msg::RuleList;
    using DiagnosticArray  = diagnostic_msgs::msg::DiagnosticArray;

    // ---- 参数与拓扑 ----
    void declareParameters();
    rclcpp::QoS makeFrameQos(const std::string& mode) const;

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

    rclcpp::Publisher<DeviceFrameArray>::SharedPtr  pubFrames_;
    rclcpp::Publisher<DiagnosticArray>::SharedPtr   pubDiag_;
    rclcpp::Publisher<RuleList>::SharedPtr          pubRules_;
    rclcpp::Service<dmp_msgs::srv::Selftest>::SharedPtr srvSelftest_;
    rclcpp::Service<dmp_msgs::srv::SetRule>::SharedPtr  srvSetRule_;
    rclcpp::Service<dmp_msgs::srv::GetRules>::SharedPtr srvGetRules_;
    std::vector<rclcpp::TimerBase::SharedPtr>        timers_;
};

} // namespace dmpbr
