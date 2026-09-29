#!/usr/bin/env bash
# 毒化实验: 第 1 轮全档(含 21s 的 worst), 第 2~4 轮只跑 mid+fast 做重复验证。
# 每轮完整输出留在 /tmp/poison_r<N>.txt, 只把表格与断言行流回来 —— 单轮不能归因,
# 要看的是同一个方向能不能重复(素材录 B-35 的教训: 判据是符号分布, 不是单次读数)。
cd ~/RosProject/dmp || exit 1
source /opt/ros/humble/setup.bash
PAT='速率比|样本不足|全饿死|饿死|^  mid_|^  fast_|^  worst_|^  PASS |^  FAIL |^PASS=|PASS=|ALL_|档 =|probe_timeout|可以下结论|不下结论|command not found'
for n in 1 2 3 4; do
  if [ "$n" = "1" ]; then LV="fast mid worst"; else LV="mid fast"; fi
  echo "########## ROUND $n (levels=$LV) ##########"
  t0=$(date +%s)
  POISON=1 POISON_LEVELS="$LV" LOG=/tmp/dmp_poison_r$n bash tools/vm_lifecycle_compose.sh \
      > /tmp/poison_r$n.txt 2>&1
  echo "RC=$? ELAPSED=$(( $(date +%s) - t0 ))s"
  grep -E "$PAT" /tmp/poison_r$n.txt
done
echo ROUNDS_DONE
