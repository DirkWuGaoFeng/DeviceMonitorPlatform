// dmp_ros2_bridge/bridge_main.cpp — 可执行入口
//
// 配置一律走 ROS2 参数 (--ros-args -p k:=v) 或 launch, 不自造命令行解析:
// 这样同一节点在 CLI / launch / 组合容器里的配置路径完全一致。
#include "dmp_ros2_bridge/bridge_node.hpp"

#include <rclcpp/rclcpp.hpp>

#include <memory>

int main(int argc, char** argv) {
    rclcpp::init(argc, argv);
    auto node = std::make_shared<dmpbr::DmpBridgeNode>(rclcpp::NodeOptions());
    rclcpp::spin(node);
    rclcpp::shutdown();
    return 0;
}
