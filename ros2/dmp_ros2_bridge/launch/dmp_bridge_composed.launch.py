#!/usr/bin/env python3
# dmp_bridge_composed.launch.py — N 台设备的桥**同处一个进程**(组件容器)
#
# 用法:
#   ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 mt:=true
#   ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 mt:=false
#   ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 \
#        gw_overrides:=bed02=192.168.109.99:9100        # 只把 bed02 的网关指向一个黑洞
#   (默认 gateway_host/port 对所有实例共用; gw_overrides 按设备覆盖 —— 机队里网关本就可能
#    按楼层/车间分组, 而它同时是毒化实验能成立的前提: 不先做到 per-device, 就造不出
#    "只坑一台"的场景, 那个场景是目前唯一能让单线程容器真正输掉边界。)
#
# 为什么要这个文件 (面试角度这才是重点):
#   * 独立进程模型 (dmp_bridge.launch.py) 每台设备一个进程: 隔离性好, 但 50 台设备
#     = 50 个进程 + 50 份 DDS 参与者, 内存和发现协议的开销都是线性的。
#   * 组件模型把 N 个桥塞进一个容器: 共享一份 DDS 参与者, 切换只需要 launch 参数。
#   * 代价是一条**并发契约**: 同一个 SingleThreaded 容器里, 一个回调阻塞 = 全容器阻塞。
#     本桥的 tickRules/set_rule 会对网关做**同步** RPC (RULES 多行回读带约 86ms), 属于会阻塞的
#     那类回调 —— 所以节点里刻意分了 grpData_/grpCtl_ 两个回调组, 并用 mt:= 在 ST/MT 容器间切。
#   * 但 mt 到底买到了什么? **本轮 A/B 没测出差别** (2026-09-29 实测, 详见 ADR-003):
#     把 bed01 的控制面打到持续排队 (~200 次调用, 平均往返 650ms), 同容器 bed02 的帧最大间隔
#     只从 212ms 变到 250ms (基线自然节律就≈200ms), 批数 60→60, >400ms 的停顿 0 次。
#     所以 mt:=false 这一路**不是"错误用法"**, 它是那台能把我假设证伪的对照机 ——
#     留开关而不是一句注释, 正是为了今天能被自己推翻。量法: tools/vm_lifecycle_compose.sh。
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


def _parse_overrides(raw):
    """'bed02=10.0.0.9:9100,bed03=127.0.0.1:9200' -> {'bed02': ('10.0.0.9', 9100), ...}

    写错不静默忽略: "我以为毒化了 bed02" 而实际覆盖没生效, 得到的就是整整一轮白跑的实验
    (与素材录 B-30/B-35 同族: 取证工具自己骗人)。宁可 launch 阶段就报错。
    """
    out = {}
    for item in [s.strip() for s in raw.split(',') if s.strip()]:
        dev, sep, ep = item.partition('=')
        host, csep, port = ep.rpartition(':')
        if not sep or not csep or not dev.strip() or not host.strip() or not port.isdigit():
            raise RuntimeError('gw_overrides 每项应为 device=host:port (端口为数字), 收到 %r' % item)
        out[dev.strip()] = (host.strip(), int(port))
    return out


def _container_setup(context, *args, **kwargs):
    devices_raw = perform_substitutions(context, [LaunchConfiguration('devices')])
    mt = perform_substitutions(context, [LaunchConfiguration('mt')]).strip().lower()
    gateway_host = perform_substitutions(context, [LaunchConfiguration('gateway_host')])
    gw_port_raw = perform_substitutions(context, [LaunchConfiguration('gateway_port')])
    conn_timeout = perform_substitutions(context, [LaunchConfiguration('connect_timeout_ms')])
    overrides = _parse_overrides(perform_substitutions(context, [LaunchConfiguration('gw_overrides')]))
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
    # 覆盖一个不存在的设备 = 拼错了设备名。不拦的话它会静默地不对任何东西生效。
    unknown = sorted(set(overrides) - set(devices))
    if unknown:
        raise RuntimeError('gw_overrides 里的设备不在 devices 里: %s (devices=%s)'
                           % (','.join(unknown), devices_raw))
    if not conn_timeout.strip().lstrip('-').isdigit():
        raise RuntimeError('connect_timeout_ms 要是整数 (0=不设限), 收到 %r' % conn_timeout)
    conn_timeout_ms = int(conn_timeout)
    if conn_timeout_ms < 0:
        raise RuntimeError('connect_timeout_ms 不能为负 (要旧行为请传 0), 收到 %d' % conn_timeout_ms)
    if not gw_port_raw.strip().isdigit():
        raise RuntimeError('gateway_port 要是数字端口, 收到 %r' % gw_port_raw)
    # 端口统一成 int 再入参: C++ 侧声明的是 declare_parameter<int>, 给字符串就靠 YAML 猜类型。
    default_ep = (gateway_host, int(gw_port_raw))
    endpoints = [overrides.get(dev, default_ep) for dev in devices]

    descriptions = [
        ComposableNode(
            package='dmp_ros2_bridge',
            plugin=PLUGIN,
            name='dmp_bridge_' + dev,       # 生命周期命令用的就是这个名字
            namespace='',
            parameters=[{
                'device_id': dev,
                'gateway_host': ep[0],
                'gateway_port': ep[1],
                'connect_timeout_ms': conn_timeout_ms,   # 整数直接入参, 不让 YAML 猜类型
                'topic_prefix': topic_prefix,
                'frame_qos': frame_qos,
            }],
        )
        for dev, ep in zip(devices, endpoints)
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
        DeclareLaunchArgument('gw_overrides', default_value='',
                              description='按设备覆盖网关端点: bed02=host:port,bed03=host:port'),
        DeclareLaunchArgument('connect_timeout_ms', default_value='1000',
                              description='网关 connect 等待上限 ms; 0=不设限(仅取证对照用)'),
        DeclareLaunchArgument('topic_prefix', default_value='/dmp'),
        DeclareLaunchArgument('frame_qos', default_value='sensor'),
        OpaqueFunction(function=_container_setup),
    ])
