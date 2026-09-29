#!/bin/bash
# 重复跑已提交的验收脚本，只提取 A/B 两行 + 队列行，用于判断 >400ms 停顿是否与容器类型相关。
cd ~/RosProject/dmp || exit 1
for i in 1 2 3; do
  echo "########## REPEAT $i ##########"
  bash tools/vm_lifecycle_compose.sh 2>&1 | grep -E '^(  mt |  st |        bed01|        \[|  实际结果|PASS=|ALL_)'
done
echo "REPEAT_DONE"
