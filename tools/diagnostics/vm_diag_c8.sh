#!/usr/bin/env bash
# 一次性诊断: 为什么 active 期 recorder 得到 0 批, 以及组合容器里两个组件到底有没有装上
set +u
set +e
source /opt/ros/humble/setup.bash
cd "$HOME/RosProject/dmp" || exit 1
source install/setup.bash
LOG=/tmp/dmp_diag; rm -rf "$LOG"; mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service" 2>/dev/null
pkill -f bridge_node 2>/dev/null; pkill -f component_container 2>/dev/null
sleep 1

gwcmd() {
  python3 - "$1" "$2" "$3" <<'PY'
import socket, sys
host, port, cmd = sys.argv[1], int(sys.argv[2]), sys.argv[3]
s = socket.create_connection((host, port), 2)
s.sendall((cmd + "\n").encode()); s.settimeout(1.5)
buf = b""
try:
    while True:
        d = s.recv(4096)
        if not d: break
        buf += d
except Exception: pass
print(buf.decode("utf-8", "replace").strip())
PY
}
GWPORT=9100
echo "=== [1] sim + gw ==="
nohup ./build_linux/device_simulator 9000 > "$LOG/sim.log" 2>&1 & sleep 1
nohup ./build_linux/gateway_service "$GWPORT" 127.0.0.1 9000 > "$LOG/gw.log" 2>&1 & sleep 3
echo "  procs: $(pgrep -fc device_simulator) sim / $(pgrep -fc gateway_service) gw"
echo "  STATS: $(gwcmd 127.0.0.1 $GWPORT STATS)"

echo "=== [2] 组件是否注册成功 ==="
timeout 60 ros2 component types 2>&1 | grep -iE 'dmp|DmpBridge' ; echo "  (空=没注册)"

echo "=== [3] 单节点 launch(autostart:=false) + 显式转换, 然后两种手段同时采 /dmp/frames ==="
nohup ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_port:="$GWPORT" \
     autostart:=false > "$LOG/bridge.log" 2>&1 & sleep 6
timeout 30 ros2 lifecycle set /dmp_bridge_bed01 configure 2>&1 | tail -2
timeout 30 ros2 lifecycle set /dmp_bridge_bed01 activate 2>&1 | tail -2
sleep 1
echo "  state=$(timeout 20 ros2 lifecycle get /dmp_bridge_bed01)"
echo "  桥日志:"; sed 's/^/    /' "$LOG/bridge.log" | tail -15
# 手段 A: 官方 CLI (vm_e2e.sh 里验证过好几百批的那个)
timeout -s INT 10 ros2 topic echo /dmp/frames > "$LOG/echo.txt" 2>&1 &
ECHO=$!
# 手段 B: rclpy recorder (本轮新写的, 现在要看它是不是假的 0)
python3 - 8 > "$LOG/rec.json" 2>"$LOG/rec.err" <<'PY'
import json, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from dmp_msgs.msg import DeviceFrameArray
dur = float(sys.argv[1]); last, cnt = {}, {}
def cb(m):
    t = time.monotonic(); fid = m.header.frame_id or '?'
    if fid in last: cnt['_maxgap'] = max(cnt.get('_maxgap', 0.0), (t - last[fid]) * 1000.0)
    last[fid] = t; cnt[fid] = cnt.get(fid, 0) + 1
rclpy.init()
n = Node('dmp_gap_probe')
sub = n.create_subscription(DeviceFrameArray, '/dmp/frames', cb,
                            QoSProfile(depth=50, reliability=ReliabilityPolicy.BEST_EFFORT))
print('writer qos after match?', flush=True)
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    rclpy.spin_once(n, timeout_sec=0.1)
cnt['_matched'] = int(sub.get_publisher_count())
print(json.dumps(cnt))
PY
wait $ECHO
echo "  手段A (ros2 topic echo): seq 行数=$(grep -c 'seq:' "$LOG/echo.txt")"
echo "  手段B (rclpy recorder): $(tail -1 "$LOG/rec.json")"
echo "  手段B stderr:"; sed 's/^/    /' "$LOG/rec.err" | tail -5
echo "  STATS 现在: $(gwcmd 127.0.0.1 $GWPORT STATS)"

echo "=== [4] 组合容器 (mt) 到底发生了什么, 日志全贴 ==="
pkill -f bridge_node 2>/dev/null; sleep 2
timeout -s INT 30 ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py \
    devices:=bed01,bed02 mt:=true gateway_port:="$GWPORT" > "$LOG/compose.log" 2>&1 &
sleep 12
echo "  nodes: $(timeout 20 ros2 node list 2>/dev/null | grep -c dmp_bridge)"
timeout 20 ros2 node list 2>/dev/null | sed 's/^/    /'
for d in bed01 bed02; do
  echo "  --- $d: configure/activate ---"
  timeout 30 ros2 lifecycle set /dmp_bridge_$d configure 2>&1 | tail -2
  timeout 30 ros2 lifecycle set /dmp_bridge_$d activate 2>&1 | tail -2
done
sleep 3
echo "  states: $(for d in bed01 bed02; do printf '%s=%s ' $d "$(timeout 15 ros2 lifecycle get /dmp_bridge_$d 2>/dev/null)"; done)"
timeout -s INT 8 ros2 topic echo /dmp/frames > "$LOG/echo2.txt" 2>&1
echo "  组合场景 echo seq 行数=$(grep -c 'seq:' "$LOG/echo2.txt")  frame_id 种类=$(grep -o "frame_id: '[^']*'" "$LOG/echo2.txt" | sort -u | tr '\n' ' ')"
echo "  容器日志:"; sed 's/^/    /' "$LOG/compose.log" | tail -25
pkill -f component_container 2>/dev/null; pkill -f bridge_node 2>/dev/null
kill $(pgrep -f device_simulator) $(pgrep -f gateway_service) 2>/dev/null
echo "DIAG_DONE"
