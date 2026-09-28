#!/usr/bin/env python3
# dmp_bridge_composed.launch.py — N 台设备的桥**同处一个进程**(组件容器)
#
# 用法:
#   ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 mt:=true
#   ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 mt:=false
#   (gateway_host/gateway_port 对所有实例共用; 本项目的网关是单点汇聚, 这样够用)
#
# 为什么要这个文件 (面试角度这才是重点):
#   * 独立进程模型 (dmp_bridge.launch.py) 每台设备一个进程: 隔离性好, 但 50 台设备
#     = 50 个进程 + 50 份 DDS 参与者, 内存和发现协议的开销都是线性的。
#   * 组件模型把 N 个桥塞进一个容器: 共享一份 DDS 参与者, 切换只需要 launch 参数。
#   * 代价是一条**并发契约**: 同一个 SingleThreaded 容器里, 一个回调阻塞 = 全容器阻塞。
#     本桥的 tickRules/set_rule 会对网关做**同步** RPC (RULES 多行回读带约 80ms 静默期),
#     正好是会造成"头阻塞"的那类回调 —— 所以节点里刻意分了 grpData_/grpCtl_ 两个回调组,
#     并且必须配 mt:=true 才有效 (两个回调组喂给单线程容器 = 白写)。
#     A/B 数字请用 tools/vm_lifecycle_compose.sh 量, 别听我说。
#
# 关于生命周期 (这是 Humble 的真实边界, 不是本项目偷懒):
#   dmp_bridge.launch.py 里那套 OnProcessStart -> ChangeState 自动激活链**在这里用不上**:
#   launch_ros 的 ChangeState 事件靠 lifecycle_node_matcher 匹配 launch 内的
#   LifecycleNode **action**, 而组合节点是容器进程自己 dlopen 出来的, launch 层没有对应
#   action 可匹配。所以组合场景的 configure/activate 必须由外部驱动:
#       ros2 lifecycle set /dmp_bridge_bed01 configure
#       ros2 lifecycle set /dmp_bridge_bed01 activate
#   本文件刻意不在 launch 里偷偷用 ExecuteProcess 去补这一层 ——
#   那会把"谁在驱动状态机"藏进 launch 文件里, 排查时反而看不见。脚本里显式写更诚实。
#
# 参数解析口径: 用 OpaqueFunction 而不是 PythonExpression, 因为 devices 是**运行期**才知道
#   长度的逗号列表, 静态 substitution 无法展开成 N 个 ComposableNode。
#   (Humble 实测: perform_substitutions 的第二个形参要的是 Substitution **列表**。)
from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, OpaqueFunction
from launch.substitutions import LaunchConfiguration
from launch.utilities import perform_substitutions
from launch_ros.actions import ComposableNodeContainer
from launch_ros.descriptions import ComposableNode

# 与 CMakeLists.txt 里 rclcpp_components_register_node(PLUGIN ...) 必须完全一致
PLUGIN = 'dmpbr::DmpBridgeNode'


def _container_setup(context, *args, **kwargs):
    devices_raw = perform_substitutions(context, [LaunchConfiguration('devices')])
    mt = perform_substitutions(context, [LaunchConfiguration('mt')]).strip().lower()
    gateway_host = LaunchConfiguration('gateway_host')
    gateway_port = LaunchConfiguration('gateway_port')
    topic_prefix = LaunchConfiguration('topic_prefix')
    frame_qos = LaunchConfiguration('frame_qos')

    devices = [d.strip() for d in devices_raw.split(',') if d.strip()]
    if not devices:
        raise RuntimeError('devices 解析为空: 传入 devices:=bed01,bed02 这样的逗号列表')
    if len(set(devices)) != len(devices):
        # 同一容器里两个同名组件: 后一个 load 失败, 现象是"只有一台设备有数据"。
        # 与其让容器抛一句含糊的 load 失败, 不如在 launch 阶段说清是谁重了。
        dup = sorted({d for d in devices if devices.count(d) > 1})
        raise RuntimeError('devices 有重复项, 同容器节点名会冲突: %s' % ','.join(dup))

    descriptions = [
        ComposableNode(
            package='dmp_ros2_bridge',
            plugin=PLUGIN,
            name='dmp_bridge_' + dev,       # 生命周期命令用的就是这个名字
            namespace='',
            parameters=[{
                'device_id': dev,
                'gateway_host': gateway_host,
                'gateway_port': gateway_port,
                'topic_prefix': topic_prefix,
                'frame_qos': frame_qos,
            }],
        )
        for dev in devices
    ]

    # 实测: Humble 的 component_container_mt 连 --help 都不认(会把 --help 当 ROS 参数,
    # 直接把容器起起来直到收到信号), 所以线程数不给参数, 用 MultiThreadedExecutor 默认值。
    container_exec = 'component_container_mt' if mt in ('true', '1', 'yes') else 'component_container'

    return [ComposableNodeContainer(
        name='dmp_bridge_container',
        namespace='',
        package='rclcpp_components',
        executable=container_exec,
        output='screen',
        composable_node_descriptions=descriptions,
    )]


def generate_launch_description():
    return LaunchDescription([
        DeclareLaunchArgument('devices', default_value='bed01,bed02',
                              description='逗号分隔的设备列表, 每台一个组件实例'),
        DeclareLaunchArgument('mt', default_value='true',
                              description='true=多线程容器(两个回调组才有意义); false=单线程(用来复现头阻塞)'),
        DeclareLaunchArgument('gateway_host', default_value='127.0.0.1'),
        DeclareLaunchArgument('gateway_port', default_value='9100'),
        DeclareLaunchArgument('topic_prefix', default_value='/dmp'),
        DeclareLaunchArgument('frame_qos', default_value='sensor'),
        OpaqueFunction(function=_container_setup),
    ])
