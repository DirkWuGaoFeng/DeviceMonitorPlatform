#!/usr/bin/env bash
# e1 探测: VM 上有哪些"真实 ROS2 生态消费者"可用, 决定下游联动这一格的断言面。
# 只读探测, 不起被试节点, 不碰 ~/RosProject/dmp 的工作区。
# 注意顺序(素材录 B-14 的同族复发): `set -u` 必须在 source 之后。
# ROS 的 setup.bash 会读非交互 shell 里未绑定的 $PS1, 而非交互 shell 碰到 unbound 直接退出整脚本 ——
# 上一次我把 2>/dev/null 贴在 source 上, 于是拿到一个“退出 1 但一个字节都不输出”的现场。
source /opt/ros/humble/setup.bash
set -u

echo "ROS_DISTRO=$ROS_DISTRO  prefix=$ROS_LOCALHOST_ONLY"
echo "--- 关键包在不在 ---"
for p in rosbag2 rosbag2_py rosbag2_storage_default_plugins \
         diagnostic_aggregator diagnostic_common_diagnostics \
         topic_to_tf_adapter tf2_tools; do
  if ros2 pkg prefix "$p" >/dev/null 2>&1; then
    echo "HAVE    $p -> $(ros2 pkg prefix $p)"
  else
    echo "MISSING $p"
  fi
done

echo "--- rosbag2 存储后端 ---"
ros2 bag info --help 2>&1 | head -3
python3 -c 'import rosbag2_py; print("rosbag2_py OK", rosbag2_py.__file__)' 2>&1 | tail -1

echo "--- 已注册的 bag 插件 ---"
ros2 bag record --help 2>&1 | grep -i -e storage -e plugin | head -5

echo "--- apt 能不能装 diagnostic_aggregator(只查不装) ---"
apt-cache policy ros-humble-diagnostic-aggregator 2>&1 | head -6

echo "--- dmp 工作区装好的包 ---"
if [ -d "$HOME/RosProject/dmp/install" ]; then
  ls "$HOME/RosProject/dmp/install" 2>/dev/null | tr '\n' ' '; echo
fi
echo PROBE_DONE
