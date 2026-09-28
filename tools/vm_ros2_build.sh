#!/usr/bin/env bash
# vm_ros2_build.sh — colcon 构建 dmp_msgs + dmp_ros2_bridge, 并跑桥的纯函数层断言
#
# 用法: bash ~/vm_ros2_build.sh   (在仓库同级放好 dmp.bundle, 或先跑 vm_native_build.sh)
# 前提: Ubuntu 22.04 + ROS2 Humble 已 source。
set +u
set +e

source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
REPO="$DMP_HOME/dmp"
BUNDLE="${BUNDLE:-$HOME/dmp.bundle}"
cd "$REPO" || exit 1

echo "=== [0] sync from bundle (fast-forward only) ==="
# 守卫不能省: bundle 的前置提交不在本机时, `git fetch` 会报
# "Repository lacks these prerequisite commits", 但后面的 colcon build 仍会绿着跑完 ——
# 你拿到的是一份"旧代码编译通过"的假绿报告。症状与素材录 B-23 同族(前提不成立但工具不拦)。
# 口径用"HEAD 必须等于 FETCH_HEAD", 而不是"HEAD 变了": 后者在已同步过时会误报。
echo "  before: HEAD=$(git rev-parse --short HEAD)"
git fetch "$BUNDLE" main || { echo "  SYNC_FAIL: fetch $BUNDLE 失败 —— 重新打一个以本机 HEAD 为前置的 bundle"; exit 3; }
git merge --ff-only FETCH_HEAD 2>&1 | tail -3
[ "$(git rev-parse HEAD)" = "$(git rev-parse FETCH_HEAD)" ] || { echo "  SYNC_FAIL: HEAD 没真的前移到 FETCH_HEAD"; exit 4; }
echo "  after : HEAD=$(git rev-parse --short HEAD)  <- 下面构建的就是这份"
grep -n 'project(dmp_msgs' ros2/dmp_msgs/CMakeLists.txt

# 只清理本脚本自己生成的构建产物, 不碰任何源码
rm -rf build/dmp_msgs build/dmp_ros2_bridge install/dmp_msgs install/dmp_ros2_bridge

echo "=== [1] colcon build ==="
colcon build --base-paths ros2 --packages-select dmp_msgs dmp_ros2_bridge \
  --event-handlers console_cohesion+ 2>&1 | tail -40

echo "=== [2] artifacts ==="
ls install/dmp_ros2_bridge/lib/dmp_ros2_bridge/ 2>&1 | tail -3
ls install/dmp_msgs/lib/ 2>&1 | head -4

echo "=== [3] colcon test (rule_mapping 断言) ==="
colcon test --base-paths ros2 --packages-select dmp_ros2_bridge 2>&1 | tail -6
colcon test-result --verbose 2>&1 | tail -6

echo "=== [4] interface sanity ==="
source install/setup.bash 2>/dev/null
ros2 interface show dmp_msgs/msg/DeviceFrame 2>&1 | head -6
ros2 interface show dmp_msgs/srv/SetRule 2>&1 | head -6
echo "SCRIPT_DONE"
