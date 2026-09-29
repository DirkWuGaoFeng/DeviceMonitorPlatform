#!/usr/bin/env bash
# 验证"现场发现"这把新尺子: 连跑 2 轮 fast 档。
# 为什么必须 2 轮: 那条 FAIL 只在第 2 轮以后出现(第 1 轮前面没有可残留的东西),
# 单轮跑绿了什么也证明不了。
# 要看的是两件事, 分得很清:
#   a) [1] 的负断言在换成现场发现之后还红不红  -> 判"缓存是不是成因"
#   b) diag_graph 那两行: 现场与 daemon 两个读数是否不一致 -> 判"有没有一次退出没道别"
cd ~/RosProject/dmp || exit 1
source /opt/ros/humble/setup.bash
PAT='^  PASS |^  FAIL |PASS=|ALL_|图诊断|档 =|SCRIPT_DONE'
for n in 1 2; do
  echo "########## GROUND-ROUND $n ##########"
  t0=$(date +%s)
  POISON=1 POISON_LEVELS="fast" LOG=/tmp/dmp_ground_r$n bash tools/vm_lifecycle_compose.sh \
      > /tmp/ground_r$n.txt 2>&1
  echo "RC=$? ELAPSED=$(( $(date +%s) - t0 ))s"
  grep -E "$PAT" /tmp/ground_r$n.txt
done
echo GROUND_DONE
