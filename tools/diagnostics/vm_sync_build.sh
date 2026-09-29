#!/usr/bin/env bash
# 同步 bundle 后在 VM 里 colcon build + colcon test, 全量日志落 /tmp/c8_build.log
# (Windows 侧由 run_vm_rounds.ps1 scp 到 /tmp 后调用 —— 不要指望它在仓库里已随 fetch 到位)
source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
echo "=== [0] sync (fast-forward only) ==="
echo "VM_HEAD_BEFORE=$(git rev-parse --short HEAD)"
# fetch 失败必须立即退: bundle 的前置提交不在本机时 git fetch 会报
# "Repository lacks these prerequisite commits", 但后面的 colcon build 仍会照旧绿着跑完 ——
# 于是得到一份"旧代码编译通过"的假绿报告。守卫比人工盯日志可靠。
git fetch "$HOME/dmp.bundle" main || { echo "SYNC_FAIL: fetch 不了, 不要继续构建"; exit 3; }
git merge --ff-only FETCH_HEAD 2>&1 | tail -3
echo "HEAD=$(git rev-parse --short HEAD)"
ls -1 ros2/dmp_ros2_bridge/src/
test -f ros2/dmp_ros2_bridge/src/bridge_component.cpp || { echo "SYNC_FAIL: 新文件不在, HEAD 没真的前移"; exit 4; }
rm -rf build/dmp_msgs build/dmp_ros2_bridge install/dmp_msgs install/dmp_ros2_bridge
echo "=== [1] colcon build ==="
colcon build --base-paths ros2 --packages-select dmp_msgs dmp_ros2_bridge \
  --event-handlers console_direct+ > /tmp/c8_build.log 2>&1
echo "BUILD_RC=$?"
echo "--- 错误/警告(前 60 行) ---"
grep -nE 'error|Error|undefined|警告|warning:' /tmp/c8_build.log | head -60
echo "--- 结尾 40 行 ---"
tail -40 /tmp/c8_build.log
echo "=== [2] artifacts ==="
ls -1 install/dmp_ros2_bridge/lib/dmp_ros2_bridge/ 2>&1
ls -1 install/dmp_ros2_bridge/lib/ 2>&1 | grep -i dmp_bridge
echo "=== [3] colcon test ==="
colcon test --base-paths ros2 --packages-select dmp_ros2_bridge 2>&1 | tail -6
colcon test-result --verbose 2>&1 | tail -6
echo "SCRIPT_DONE"
