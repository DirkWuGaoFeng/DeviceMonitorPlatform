#!/usr/bin/env bash
# vm_inventory.sh - 盘点 ROS2 开发虚拟机环境 (只读诊断, 不改动任何内容)
# 用法(Windows 侧): Get-Content -Raw tools\vm_inventory.sh | ssh -i ~/.ssh/id_rsa dirk@<VM_IP> bash -s
#   注意: 管道前先去掉 CR, 否则 bash 会报 $'\r'
# 注意: 不能用 set -u / set -e —— ROS 的 setup.bash 内部引用未定义变量,
# 在 set -u 下会直接终止脚本 (实测症状: 输出在 source 之后截断)
set +u
set +e
echo "=== [1] ROS 发行版 ==="
ls /opt/ros/ 2>/dev/null || echo "无 /opt/ros"

echo "=== [2] ROS2 环境 (source humble 后) ==="
if source /opt/ros/humble/setup.bash 2>/dev/null; then
  echo "ROS_DISTRO=$ROS_DISTRO  RMW=$RMW_IMPLEMENTATION"
  ros2 pkg prefix rclcpp 2>/dev/null
else
  echo "source /opt/ros/humble 失败"
fi

echo "=== [3] 已装 ros-humble 包总数 + 关键包 ==="
dpkg -l 2>/dev/null | grep -c '^ii.*ros-humble'
for p in ros-humble-navigation2 ros-humble-nav2-bringup ros-humble-slam-toolbox \
         ros-humble-robot-localization ros-humble-ros2-control ros-humble-ros2-launch \
         ros-humble-xacro ros-humble-tmux ros-humble-gazebo-ros-pkgs \
         ros-humble-turtlebot3 ros-humble-turtlebot3-msgs; do
  if dpkg -l 2>/dev/null | grep -q "^ii  $p "; then echo "  [有] $p"; else echo "  [无] $p"; fi
done

echo "=== [4] RosProject 目录树 (depth 3, 跳过 build/install/log) ==="
find ~/RosProject -maxdepth 3 -type d \
  -not -path '*/build*' -not -path '*/install*' -not -path '*/log*' 2>/dev/null | head -50

echo "=== [5] 自研代码 (排除第三方 clone 与构建产物) ==="
find ~/RosProject -name '*.cpp' -o -name '*.py' 2>/dev/null \
  | grep -vE '/(build|install|log)/' \
  | grep -vE '/src/(navigation2|slam_toolbox|robot_localization|m-explore|ros2_control|gazebo)' \
  | head -30

echo "=== [6] 自建 ROS 包的 package.xml / CMakeLists 摘要 ==="
find ~/RosProject -maxdepth 5 -name package.xml 2>/dev/null \
  | grep -vE '/(build|install)/' | head -15 | while read -r f; do
    echo "--- $f"; grep -m4 -E '<name>|<version>|depend' "$f" | sed 's/^ *//'
  done

echo "=== [7] Nav2 动作客户端源码位置 ==="
grep -rl -E 'NavigateToPose|FollowWaypoints|rclcpp_action' ~/RosProject --include='*.cpp' --include='*.py' 2>/dev/null \
  | grep -vE '/(build|install)/' | head -10

echo "=== [8] 资源 ==="
nproc; free -h | sed -n '1,2p'; df -h ~ | tail -1
echo "=== 完成 ==="
