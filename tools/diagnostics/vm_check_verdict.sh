#!/bin/bash
# 只验两件事, 不重跑 2.5 分钟的整机验收:
#   1) bash -n 语法
#   2) 脚本末尾那段"动态结论行" python —— 抽出来用第 7 轮留下的 /tmp/dmp_lifecycle 直接喂它
cd ~/RosProject/dmp || exit 1
bash -n tools/vm_lifecycle_compose.sh && echo BASH_SYNTAX_OK || exit 2
start=$(grep -n '^python3 - "\$LOG" <<' tools/vm_lifecycle_compose.sh | tail -1 | cut -d: -f1)
end=$(awk -v s="$start" 'NR>s && /^PY$/ {print NR; exit}' tools/vm_lifecycle_compose.sh)
echo "extract lines: $((start+1))..$((end-1))"
sed -n "$((start+1)),$((end-1))p" > /tmp/verdict.py
python3 /tmp/verdict.py /tmp/dmp_lifecycle
echo "rc=$?  (输入是第 7 轮的 json, 期望差值 mt +49.6 / st +45.4)"
echo "CHECK_DONE"
