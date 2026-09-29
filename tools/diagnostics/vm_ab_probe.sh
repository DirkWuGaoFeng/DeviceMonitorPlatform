#!/usr/bin/env bash
# 一次性探针: 只跑组合容器 A/B, 把并发窗口 K 做成参数, 用来判"队列深度会不会饿死数据面"。
# 假设对撞:
#   H1 (我原先写进 ADR 的说法): 单线程容器里控制面持续排队 => 同容器另一台设备的帧被拖住,
#       帧间隔随队列深度增长。
#   H2 (K=8 实测后提出的替代解释): 损害被"单个回调的时长"(网关 RULES 回读 ~86ms) 封顶,
#       与队列深度无关 => 加深窗口只会让控制面自己排队, 数据面不动。
# 判别: 若 st 在 K=32 时的最大间隔明显大于 K=8, H1 活; 若仍是 ~200ms 自然节律, H2 活。
set +u
set +e
source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG=/tmp/dmp_abprobe
GW_PORT="${GW_PORT:-9100}"
SIM_PORT="${SIM_PORT:-9000}"
KLIST="${KLIST:-8 32}"
rm -rf "$LOG"; mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f component_container 2>/dev/null
sleep 1

recorder() {  # recorder <秒> <json> <阈值ms>
  timeout -s INT $(( $1 + 3 )) python3 - "$1" "${3:-400}" > "$2" 2>/dev/null <<'PY'
import json, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from dmp_msgs.msg import DeviceFrameArray
dur = float(sys.argv[1]); thr = float(sys.argv[2])
last, cnt, gap, stall = {}, {}, {}, {}
def cb(m):
    t = time.monotonic(); fid = m.header.frame_id or '?'
    if fid in last:
        g = (t - last[fid]) * 1000.0
        if g > gap.get(fid, 0.0): gap[fid] = g
        if g > thr: stall[fid] = stall.get(fid, 0) + 1
    last[fid] = t; cnt[fid] = cnt.get(fid, 0) + 1
rclpy.init(); n = Node('dmp_gap_probe')
n.create_subscription(DeviceFrameArray, '/dmp/frames', cb,
                      QoSProfile(depth=50, reliability=ReliabilityPolicy.BEST_EFFORT))
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    rclpy.spin_once(n, timeout_sec=0.05)
print(json.dumps({'count': cnt, 'max_gap_ms': {k: round(v,1) for k,v in gap.items()},
                  'stalls': {k: v for k,v in stall.items()}}))
PY
}

hammer() {  # hammer <日志> <秒> <并发窗口>
  local out=$1 dur=$2 kwin=$3
  timeout -s INT $(( dur + 8 )) python3 - "$dur" "$kwin" > "$out" 2>&1 <<'PY'
import sys, time
import rclpy
from rclpy.node import Node
from dmp_msgs.srv import SetRule
dur, kwin = float(sys.argv[1]), int(sys.argv[2])
rclpy.init(); n = Node('dmp_ctl_hammer')
cl = n.create_client(SetRule, '/dmp/bed01/set_rule')
if not cl.wait_for_service(timeout_sec=10):
    print('NO_SERVICE'); sys.exit(1)
t0 = time.monotonic(); sent = 0; inflight = []; lat = []; peak = 0
def reap():
    keep = []
    for f, t1 in inflight:
        if f.done(): lat.append((time.monotonic() - t1) * 1000.0)
        else: keep.append((f, t1))
    del inflight[:]; inflight.extend(keep)
while time.monotonic() - t0 < dur:
    while len(inflight) < kwin:
        req = SetRule.Request(); req.kind = 2
        req.low = 45.0 + (sent % 5); req.high = 115.0; req.message = 'hammer'
        inflight.append((cl.call_async(req), time.monotonic())); sent += 1
    rclpy.spin_once(n, timeout_sec=0.02); reap()
    if len(inflight) > peak: peak = len(inflight)
deadline = time.monotonic() + 8
while inflight and time.monotonic() < deadline:
    rclpy.spin_once(n, timeout_sec=0.05); reap()
print('calls=%d peak_inflight=%d ctl_avg_ms=%.1f ctl_max_ms=%.1f' % (
    len(lat), peak, sum(lat)/max(1,len(lat)), max(lat) if lat else 0.0))
PY
}

echo "=== [0] simulator + gateway ==="
nohup ./build_linux/device_simulator "$SIM_PORT" > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service "$GW_PORT" 127.0.0.1 "$SIM_PORT" > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2

one() {  # one <mt|st> <K>
  local mt=$1 kwin=$2 tag="${1}_K${2}"
  pkill -f component_container 2>/dev/null; sleep 2
  nohup ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py \
        devices:=bed01,bed02 mt:="$mt" gateway_port:="$GW_PORT" > "$LOG/$tag.log" 2>&1 &
  local lp=$!
  sleep 10
  for d in bed01 bed02; do
    timeout 30 ros2 lifecycle set /dmp_bridge_$d configure >/dev/null 2>&1
    timeout 30 ros2 lifecycle set /dmp_bridge_$d activate  >/dev/null 2>&1
  done
  sleep 3
  recorder 10 "$LOG/gap_${tag}_quiet.json" 400 >/dev/null 2>&1
  hammer "$LOG/hammer_$tag.log" 16 "$kwin" &
  local hp=$!
  sleep 2
  recorder 12 "$LOG/gap_${tag}_busy.json" 400 >/dev/null 2>&1
  wait "$hp" 2>/dev/null
  python3 - "$tag" "$LOG" <<'PY'
import json, sys
tag, d = sys.argv[1], sys.argv[2]
def g(f):
    try:
        j = json.load(open(f"{d}/gap_{tag}_{f}.json"))
        return (j['count'].get('bed02', 0), j['max_gap_ms'].get('bed02', 0.0),
                j.get('stalls', {}).get('bed02', 0))
    except Exception as e:
        return -1, -1.0, -1
qn, qg, qs = g('quiet'); bn, bg, bs = g('busy')
h = open(f"{d}/hammer_{tag}.log").read().strip().splitlines()[-1:] or ['(缺)']
print(f"  {tag:8s}: 空载 n={qn:3d} max={qg:6.1f}ms stall={qs} | 打满 n={bn:3d} max={bg:6.1f}ms stall={bs} | {h[0]}")
PY
  kill "$lp" 2>/dev/null; pkill -f component_container 2>/dev/null; sleep 2
}

echo "=== [1] A/B: 队列深度 K 对数据面的影响 (被测量始终是 bed02) ==="
for k in $KLIST; do one true  "$k"; done
for k in $KLIST; do one false "$k"; done
kill "$GW" "$SIM" 2>/dev/null
echo "PROBE_DONE"
