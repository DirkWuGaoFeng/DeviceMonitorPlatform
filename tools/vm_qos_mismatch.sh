#!/usr/bin/env bash
# vm_qos_mismatch.sh — 复现并量测 ROS2 QoS 不匹配导致的"静默无数据"
#
# 结论 (本脚本用实测钉死, 不是背文档):
#   reliability 是"发布端能力 >= 订阅端要求"才兼容:
#     发布 RELIABLE   + 订阅 BEST_EFFORT -> 兼容 (退化成不重传, 仍能收到)
#     发布 BEST_EFFORT + 订阅 RELIABLE   -> 不兼容, 一个都收不到, 且没有任何报错
#   所以"连得上、订阅列表里有、echo 就是不出东西"的那个方向是后者。
#
# 用法: bash tools/vm_qos_mismatch.sh   (需先跑通 vm_native_build.sh 与 vm_ros2_build.sh)
set +u
set +e

source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG="${LOG:-/tmp/dmp_qos}"
mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f "dmp_ros2_bridge/bridge_node"       2>/dev/null
sleep 1

nohup ./build_linux/device_simulator 9000 > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service 9100 127.0.0.1 9000 > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2

# 计时口径: 用 SIGINT 而非默认 SIGTERM —— 被 SIGTERM 杀掉的 python 进程不flush,
# 块缓冲里的数据会整体丢失, 于是"0 条"可能是取证手段自己造成的假象。
probe() {  # probe <outfile> <订阅可靠性> <秒>
  timeout -s INT "$3" ros2 topic echo /dmp/frames --qos-reliability "$2" > "$1" 2>&1
  echo "  -> $1: frame_lines=$(grep -c 'seq:' "$1") lines=$(wc -l < "$1")"
}

start_bridge() {  # start_bridge <frame_qos>
  pkill -f "dmp_ros2_bridge/bridge_node" 2>/dev/null; sleep 1
  nohup ros2 run dmp_ros2_bridge bridge_node --ros-args \
    -p device_id:=bed01 -p gateway_port:=9100 -p frame_qos:="$1" > "$LOG/bridge_$1.log" 2>&1 &
  BR=$!; sleep 8
  kill -0 "$BR" 2>/dev/null && echo "bridge(frame_qos=$1)=alive" || echo "bridge(frame_qos=$1)=DEAD"
  timeout -s INT 15 ros2 topic info /dmp/frames --verbose 2>&1 | grep -E 'Reliability|Durability' | head -2
}

echo "=== [A] 发布端 = sensor (BEST_EFFORT) ==="
start_bridge sensor
echo "[A1] 订阅端 reliable (要求重传, 发布端给不了) 8s:"
probe "$LOG/A_rel.log" reliable 8
echo "[A2] 订阅端 best_effort 8s:"
probe "$LOG/A_be.log" best_effort 8

echo "=== [B] 发布端 = reliable ==="
start_bridge reliable
echo "[B1] 订阅端 best_effort 8s (文档上叫'兼容, 只是不重传'):"
probe "$LOG/B_be.log" best_effort 8
echo "[B2] 订阅端 reliable 8s:"
probe "$LOG/B_rel.log" reliable 8

echo "=== 判读 ==="
printf 'A1 pub=BE sub=RELIABLE  : %s 条  <- 期望 0 (不兼容, 静默)\n' "$(grep -c 'seq:' "$LOG/A_rel.log")"
printf 'A2 pub=BE sub=BESTEFFORT: %s 条  <- 期望 >0\n' "$(grep -c 'seq:' "$LOG/A_be.log")"
printf 'B1 pub=REL sub=BESTEFFORT: %s 条  <- 期望 >0 (兼容)\n' "$(grep -c 'seq:' "$LOG/B_be.log")"
printf 'B2 pub=REL sub=RELIABLE  : %s 条  <- 期望 >0\n' "$(grep -c 'seq:' "$LOG/B_rel.log")"

kill "$BR" "$GW" "$SIM" 2>/dev/null
echo "SCRIPT_DONE"
