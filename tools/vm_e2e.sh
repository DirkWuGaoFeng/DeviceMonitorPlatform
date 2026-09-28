#!/usr/bin/env bash
# vm_e2e.sh — VM 内全链路冒烟: device_simulator -> gateway_service -> bridge_node -> ROS2
#
# 验证四件事:
#   上行帧流   /dmp/frames       (协议保真层, 帧内 CRC 不过的帧不得成为消息)
#   标准视图   /diagnostics      (diagnostic_msgs, 链路健康 + 每通道现值判级)
#   锁存配置   /dmp/rules        (transient_local: 晚起的订阅者也该看到当前阈值)
#   下行写路径 set_rule 服务      + 从网关侧独立回读, 证明阈值真的落在网关上
#
# 用法: bash ~/vm_e2e.sh
# 前提: vm_native_build.sh 与 vm_ros2_build.sh 都已成功。
set +u
set +e

source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG="${LOG:-/tmp/dmp_e2e}"
GW_PORT="${GW_PORT:-9100}"
SIM_PORT="${SIM_PORT:-9000}"
mkdir -p "$LOG"

# 只清理本脚本自己拉起的东西: 按绝对路径匹配, 不碰同名他人进程
pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f "dmp_ros2_bridge/bridge_node"       2>/dev/null
sleep 1

# 文本命令走控制面: 用 python 明确收包。
# (不要用 `timeout 2 head -N <&3` —— head 的 stdout 是管道时按块缓冲, 被 timeout 杀掉会丢掉已读内容,
#  表现为"网关没回应", 实际是取证手段自己错了。)
gwcmd() {
  python3 - "$GW_PORT" "$1" <<'PY'
import socket, sys
port, cmd = int(sys.argv[1]), sys.argv[2]
s = socket.create_connection(("127.0.0.1", port), 2)
s.sendall((cmd + "\n").encode())
s.settimeout(1.5)
buf = b""
try:
    while True:
        d = s.recv(4096)
        if not d:
            break
        buf += d
except Exception:
    pass
sys.stdout.write(buf.decode("utf-8", errors="replace"))
PY
}

echo "=== [1] simulator + gateway ==="
nohup ./build_linux/device_simulator "$SIM_PORT" > "$LOG/sim.log" 2>&1 &
SIM=$!
sleep 1
nohup ./build_linux/gateway_service "$GW_PORT" 127.0.0.1 "$SIM_PORT" > "$LOG/gw.log" 2>&1 &
GW=$!
sleep 2
echo "sim_pid=$SIM gw_pid=$GW"
kill -0 "$SIM" 2>/dev/null && echo "sim=alive" || { echo "sim=DEAD"; tail -5 "$LOG/sim.log"; }
kill -0 "$GW"  2>/dev/null && echo "gw=alive"  || { echo "gw=DEAD";  tail -5 "$LOG/gw.log"; }
echo "--- sim.log ---"; tail -3 "$LOG/sim.log"
echo "--- STATS via control plane ---"; gwcmd "STATS"

echo "=== [2] bridge_node ==="
nohup ros2 run dmp_ros2_bridge bridge_node --ros-args \
  -p device_id:=bed01 -p gateway_port:="$GW_PORT" -p frame_qos:=sensor \
  -p diag_period_ms:=500 -p rule_refresh_ms:=3000 > "$LOG/bridge.log" 2>&1 &
BR=$!
sleep 8
kill -0 "$BR" 2>/dev/null && echo "bridge=alive" || echo "bridge=DEAD"
head -10 "$LOG/bridge.log"
timeout 15 ros2 node list 2>&1 | tail -4
timeout 15 ros2 topic list 2>&1 | grep -E 'dmp|diag'

echo "=== [3] uplink /dmp/frames (协议保真层) ==="
timeout 20 ros2 topic echo /dmp/frames --once 2>&1 | head -28

echo "=== [4] uplink /diagnostics (标准视图) ==="
timeout 20 ros2 topic echo /diagnostics --once 2>&1 | head -34

echo "=== [5] latched /dmp/rules ==="
timeout 20 ros2 topic echo /dmp/rules --once 2>&1 | head -16

echo "=== [6] 下行写路径 ==="
timeout 25 ros2 service call /dmp/bed01/selftest dmp_msgs/srv/Selftest "{probe: e2e}" 2>&1 | tail -4
timeout 25 ros2 service call /dmp/bed01/set_rule dmp_msgs/srv/SetRule "{kind: 2, low: 45.0, high: 115.0}" 2>&1 | tail -4
echo "--- 网关侧独立回读 (证明写进了网关, 不是桥自己记的) ---"
gwcmd "RULES"
echo "--- ROS 侧 get_rules ---"
timeout 25 ros2 service call /dmp/bed01/get_rules dmp_msgs/srv/GetRules "{}" 2>&1 | tail -3

echo "=== [7] 非法写入必须被拒 (区间倒置) ==="
timeout 25 ros2 service call /dmp/bed01/set_rule dmp_msgs/srv/SetRule "{kind: 3, low: 90.0, high: 20.0}" 2>&1 | tail -3

echo "=== cleanup ==="
kill "$BR" "$GW" "$SIM" 2>/dev/null
sleep 1
tail -8 "$LOG/bridge.log"
echo "SCRIPT_DONE"
