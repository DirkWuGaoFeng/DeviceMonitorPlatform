#!/usr/bin/env python3
# dmp_bridge.launch.py — 起一台设备的桥实例
#
# 用法:
#   ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_host:=127.0.0.1
#   ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=robot03 gateway_host:=192.168.109.1
#
# 多设备=多实例: 每台设备一个节点 + 一个 topic_prefix, 因此机队规模扩展不需要改代码。
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import Node


def generate_launch_description():
    device = LaunchConfiguration('device_id')
    return LaunchDescription([
        DeclareLaunchArgument('device_id', default_value='bed01',
                              description='设备标识, 进入 frame_id 与服务名'),
        DeclareLaunchArgument('gateway_host', default_value='127.0.0.1'),
        DeclareLaunchArgument('gateway_port', default_value='9100'),
        DeclareLaunchArgument('topic_prefix', default_value='/dmp'),
        # frame_qos: sensor=best_effort(高频遥测默认) | reliable | reliable_deep。
        # 静默无数据的错配方向是 “发布 best_effort + 订阅 reliable” (实测), 不是反过来;
        # 取证脚本: tools/vm_qos_mismatch.sh
        DeclareLaunchArgument('frame_qos', default_value='sensor'),
        DeclareLaunchArgument('batch_period_ms', default_value='50'),
        DeclareLaunchArgument('diag_period_ms', default_value='1000'),
        DeclareLaunchArgument('rule_refresh_ms', default_value='10000'),

        Node(
            package='dmp_ros2_bridge',
            executable='bridge_node',
            name=['dmp_bridge_', device],
            output='screen',
            parameters=[{
                'device_id': device,
                'gateway_host': LaunchConfiguration('gateway_host'),
                'gateway_port': LaunchConfiguration('gateway_port'),
                'topic_prefix': LaunchConfiguration('topic_prefix'),
                'frame_qos': LaunchConfiguration('frame_qos'),
                'batch_period_ms': LaunchConfiguration('batch_period_ms'),
                'diag_period_ms': LaunchConfiguration('diag_period_ms'),
                'rule_refresh_ms': LaunchConfiguration('rule_refresh_ms'),
            }],
        ),
    ])
