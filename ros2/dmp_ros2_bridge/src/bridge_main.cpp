// dmp_ros2_bridge/bridge_main.cpp — 可执行入口 (独立进程模式)
//
// 配置一律走 ROS2 参数 (--ros-args -p k:=v) 或 launch, 不自造命令行解析:
// 这样同一节点在 CLI / launch / 组合容器里的配置路径完全一致。
//
// 生命周期升级后的两点变化 (都不是可选的):
//   1) rclcpp::spin(node) 吃不了 LifecycleNode —— 它只接受 rclcpp::Node。
//      所以这里显式用 executor + get_node_base_interface()。
//   2) 刻意**不做 autostart**: 桥起来后停在 unconfigured, 不连网关也不发话题。
//      采集必须是一次显式的 TRANSITION_ACTIVATE, 留下 /transition_event 与日志证据。
//      代价要写明白: 忘了激活的症状与复盘录 B-01 完全同族 —— 节点在、话题在、不报错、零数据。
//      所以验收脚本里"configure + activate"是显式一步, 不是隐藏在这里。
#include "dmp_ros2_bridge/bridge_node.hpp"

#include <rclcpp/executors/single_threaded_executor.hpp>
#include <rclcpp/rclcpp.hpp>

#include <memory>

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<dmpbr::DmpBridgeNode>(rclcpp::NodeOptions());
    RCLCPP_INFO(node->get_logger(),
                "等待生命周期转换: ros2 lifecycle set /%s configure && ros2 lifecycle set /%s activate",
                node->get_name(), node->get_name());

    rclcpp::executors::SingleThreadedExecutor exec;
    exec.add_node(node->get_node_base_interface());
    exec.spin();

    rclcpp::shutdown();
    return 0;
}
