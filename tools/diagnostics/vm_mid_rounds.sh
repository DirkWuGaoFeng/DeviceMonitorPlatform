#!/usr/bin/env bash
# 抓现行: 连跑 2 轮 **mid 档**(3s 阻塞)。
# 为什么是 mid: 那条 FAIL 只在毒化系列(含 mid/worst)的第 2~4 轮出现, 而 fast-only 的
# 两轮验证里现场与 daemon 两路都是 0 —— 说明成因跟"长阻塞"有关, 不在 fast 档里。
# 本轮不改护栏: 留着它红, 让新加的 diag_graph 把"谁还活着、活了多久"打进日志。
cd ~/RosProject/dmp || exit 1
source /opt/ros/humble/setup.bash
PAT='^  PASS |^  FAIL |PASS=|ALL_|图诊断|档 =|SCRIPT_DONE'
for n in 1 2; do
  echo "########## MID-ROUND $n ##########"
  t0=$(date +%s)
  POISON=1 POISON_LEVELS="mid" LOG=/tmp/dmp_mid_r$n bash tools/vm_lifecycle_compose.sh \
      > /tmp/mid_r$n.txt 2>&1
  echo "RC=$? ELAPSED=$(( $(date +%s) - t0 ))s"
  grep -E "$PAT" /tmp/mid_r$n.txt
done
echo MID_DONE
