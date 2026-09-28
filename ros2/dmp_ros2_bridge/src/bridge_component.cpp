// dmp_ros2_bridge/bridge_component.cpp — 组件注册 (把节点做成可 dlopen 的插件)
//
// 为什么单独一个 .cpp: 注册宏会往这个翻译单元里塞一个 class_loader 导出符号。
// 于是 bridge_node.cpp 保持"纯节点实现", 独立进程入口 (bridge_main.cpp) 与
// 组合容器插件共用同一份实现, 不互相牵连。
//
// 注册后可用面 (Humble 实测):
//   ros2 component types            # 能列出 dmpbr::DmpBridgeNode
//   ros2 launch ... 组合容器         # 见 launch/dmp_bridge_composed.launch.py
//   rclcpp_components/component_container     (单线程, 用来复现头阻塞)
//   rclcpp_components/component_container_mt  (多线程, 配两个回调组才有效)
#include "dmp_ros2_bridge/bridge_node.hpp"

#include <rclcpp_components/register_node_macro.hpp>

RCLCPP_COMPONENTS_REGISTER_NODE(dmpbr::DmpBridgeNode)
