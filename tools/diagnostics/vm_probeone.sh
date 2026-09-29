#!/usr/bin/env bash
# 决定性一步: 跑一轮含 worst 档(21s 阻塞)的完整脚本, 结束后**不碰 daemon**, 立刻读两路。
#   daemon=1 而 现场=0  -> 真有一次退出没道别, 缓存就是那条 FAIL 的成因
#   两路都 0            -> 连"最原始的配方"也复现不了, 我不写任何因果
# 为什么必须单独做这一步: 我前面的取证探针里执行过 `ros2 daemon stop/start`, 那一下就把
# 缓存冲干净了 —— 之后连跑的 fast/mid 轮全绿只能说明"现场是干净的", 不能用来否证缓存。
# (B-38 的姊妹形态: 取证工具动过被试现场, 却把自己的动作读成被试的性质。)
cd ~/RosProject/dmp || exit 1
source /opt/ros/humble/setup.bash
echo "########## SINGLE ROUND (mid worst) ##########"
t0=$(date +%s)
POISON=1 POISON_LEVELS="mid worst" LOG=/tmp/dmp_probeone bash tools/vm_lifecycle_compose.sh \
    > /tmp/probeone.txt 2>&1
echo "RC=$? ELAPSED=$(( $(date +%s) - t0 ))s"
grep -E '^  PASS |^  FAIL |PASS=|ALL_|图诊断' /tmp/probeone.txt
echo "=== 脚本已退出后立刻读 (全程不 stop/start daemon) ==="
for k in 1 2 3; do
  echo "  第 $k 次(脚本退出后 +$(( (k-1)*15 ))s): 现场=$(timeout 30 ros2 topic list --no-daemon --spin-time 6 2>/dev/null | grep -c '^/dmp/frames$') daemon=$(timeout 20 ros2 topic list 2>/dev/null | grep -c '^/dmp/frames$') 存活容器=$(pgrep -c -f 'component_container|bridge_node')"
  [ "$k" -lt 3 ] && sleep 15
done
echo "=== daemon 缓存里到底有谁 ==="
timeout 20 ros2 node list 2>/dev/null | head -8
echo "PROBEONE_DONE"
