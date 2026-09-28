#!/usr/bin/env python3
# dmp_bridge.launch.py — 起一台设备的桥实例 (生命周期管理型节点)
#
# 用法:
#   ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_host:=127.0.0.1
#   ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=robot03 gateway_host:=192.168.109.1
#   ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 autostart:=false   # 留着做负例
#
# 多设备=多实例: 每台设备一个节点 + 一个 topic_prefix, 因此机队规模扩展不需要改代码。
#
# 关于 autostart (这是本项目的一个真实取舍, 不是偷懒):
#   * 节点自己**绝不**激活 —— 构造器里没有任何副作用, 采集只能由 TRANSITION_ACTIVATE 触发。
#   * Humble 的 launch_ros.LifecycleNode **没有** autostart 参数 (实测 inspect 过签名,
#     只有 name/namespace/**kwargs), 所以自动激活要用事件处理器手写:
#     OnProcessStart -> ChangeState(CONFIGURE), 再 OnStateTransition(configuring->inactive)
#     -> ChangeState(ACTIVATE)。这条链在日志里会留下真实的 /transition_event, 不是假激活。
#   * 第二层的 start_state/goal_state 是**必须**的: 只写 goal_state='inactive' 的话,
#     以后每次 deactivate 都会落回 inactive, 于是事件处理器把它再激活一次 ——
#     "运维按下停止, 三秒后设备自己开始采集" 就是这种 bug 的现场形态。
#   * 忘了激活的症状与素材录 B-01 同族: 节点在、话题在、不报错、零数据。
#     负例请用 tools/vm_lifecycle_compose.sh, 别靠肉眼看。
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, EmitEvent, RegisterEventHandler
from launch.conditions import IfCondition
from launch.event_handlers import OnProcessStart
from launch.events import matches_action
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import LifecycleNode
from launch_ros.event_handlers import OnStateTransition
from launch_ros.events.lifecycle import ChangeState
from lifecycle_msgs.msg import Transition


def generate_launch_description():
    device = LaunchConfiguration('device_id')
    autostart = LaunchConfiguration('autostart')

    bridge = LifecycleNode(
        package='dmp_ros2_bridge',
        executable='bridge_node',
        name=['dmp_bridge_', device],
        namespace='',                    # Humble 里 namespace 是必填关键字参数
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
    )

    configure_on_start = RegisterEventHandler(
        OnProcessStart(
            target_action=bridge,
            on_start=[
                EmitEvent(event=ChangeState(
                    lifecycle_node_matcher=matches_action(bridge),
                    transition_id=Transition.TRANSITION_CONFIGURE)),
            ],
        ),
        condition=IfCondition(autostart),
    )

    activate_after_configure = RegisterEventHandler(
        OnStateTransition(
            target_lifecycle_node=bridge,
            start_state='configuring',      # 只认"配置中->闲置"这一条边, 见文件头注释
            goal_state='inactive',
            entities=[
                EmitEvent(event=ChangeState(
                    lifecycle_node_matcher=matches_action(bridge),
                    transition_id=Transition.TRANSITION_ACTIVATE)),
            ],
        ),
        condition=IfCondition(autostart),
    )

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
        DeclareLaunchArgument('autostart', default_value='true',
                              description='true=launch 里自动 configure+activate; false=停在 unconfigured'),

        bridge,
        configure_on_start,
        activate_after_configure,
    ])
