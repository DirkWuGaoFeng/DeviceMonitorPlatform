#!/usr/bin/env bash
# vm_lifecycle_compose.sh — 生命周期升级(c8)的两问验收
#
#   Q3: "不激活就绝不采集" 是**契约**还是**愿望**?  -> [1]~[4] 用负例把它钉死
#   Q4: 两个回调组 + 多线程容器 到底买到了什么?    -> [5] 用毫秒数回答
#
# 用法(VM 内): bash tools/vm_lifecycle_compose.sh
# 前提: tools/vm_ros2_build.sh 已成功 (需要 bridge_node 与 dmp_bridge_component 都在 install 里)。
#
# 计时口径沿用 vm_realdevice_e2e.sh 的教训: 一律 timeout -s INT (SIGTERM 会丢掉块缓冲里的数据,
# 于是"0 条"可能是取证手段自己造成的假象) —— 而本脚本恰恰要测"0 条", 所以这条口径是命门。
#
# Q3 的断言为什么必须分三层, 一层都不能省:
#   unconfigured: 话题/服务**根本不存在** (publisher 与三个服务都是 on_configure 才创建的)
#   inactive    : 话题在、服务在、订阅得到, 但**零数据** (LifecyclePublisher::publish() 未激活时
#                 直接 return; 不是完全静默 —— 每个激活周期会打**一句** WARN, 但 logger 名是
#                 "LifecyclePublisher" 而不是节点名, 按节点名 grep 会看漏)
#   active      : 数据与写入都恢复
#   只测"active 能跑通"就等于没测: 那无法区分"守卫生效"和"恰好没数据"。
#
# 反向同样成立, 而且是本轮真踩到的: 首跑 [2]"inactive 零数据"PASS 了, 但它是**假 PASS** ——
# 那时 on_activate 忘了链回基类, 门禁从头到尾没开过, 这一条在任何情况下都会绿。
# 口径: **"零数据"类断言只有在同脚本能证明"该有数据时确实有"的前提下才成立**,
# 否则它测的是你的 bug, 不是守卫。所以下面 [3] 必须与 [2] 一起看, 不能各自报功。
set +u
set +e
source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG="${LOG:-/tmp/dmp_lifecycle}"
GW_PORT="${GW_PORT:-9100}"
SIM_PORT="${SIM_PORT:-9000}"
rm -rf "$LOG"; mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f bridge_node 2>/dev/null
pkill -f component_container 2>/dev/null
sleep 1
PASS=0; FAIL=0
chk() {  # chk <说明> <实际> <期望>
  if [ "$2" = "$3" ]; then echo "  PASS $1 (=$2)"; PASS=$((PASS+1));
  else echo "  FAIL $1: 实际=$2 期望=$3"; FAIL=$((FAIL+1)); fi
}

# 记录 /dmp/frames 各来源(frame_id)的到达间隔。订阅 QoS 必须 best_effort:
# 桥默认发 sensor(best_effort), 用 reliable 订阅会得到"零数据", 那测的是 QoS 不是生命周期。
recorder() {  # recorder <秒> <输出json文件>
  timeout -s INT $(( $1 + 3 )) python3 - "$1" > "$2" 2>/dev/null <<'PY'
import json, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from dmp_msgs.msg import DeviceFrameArray
dur = float(sys.argv[1])
last, cnt, gap = {}, {}, {}
def cb(m):
    t = time.monotonic(); fid = m.header.frame_id or '?'
    if fid in last:
        g = (t - last[fid]) * 1000.0
        if g > gap.get(fid, 0.0): gap[fid] = g
    last[fid] = t; cnt[fid] = cnt.get(fid, 0) + 1
rclpy.init(); n = Node('dmp_gap_probe')
n.create_subscription(DeviceFrameArray, '/dmp/frames', cb,
                      QoSProfile(depth=50, reliability=ReliabilityPolicy.BEST_EFFORT))
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    rclpy.spin_once(n, timeout_sec=0.05)
print(json.dumps({'count': cnt, 'max_gap_ms': {k: round(v, 1) for k, v in gap.items()}}))
PY
}

echo "=== [0] simulator + gateway (本地, 排除网络变量) ==="
nohup ./build_linux/device_simulator "$SIM_PORT" > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service "$GW_PORT" 127.0.0.1 "$SIM_PORT" > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2

echo "=== [1] unconfigured (autostart:=false): 采集路径必须压根不存在 ==="
NODE=dmp_bridge_bed01
nohup ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_port:="$GW_PORT" \
     autostart:=false > "$LOG/bridge_unconf.log" 2>&1 & LAUNCH=$!
sleep 6
echo "  node 在吗: $(timeout 15 ros2 node list 2>/dev/null | grep -c "/$NODE")  (期望 1: 节点本身构造即存在)"
chk "unconfigured 时 /dmp/frames 不在 topic list" \
    "$(timeout 15 ros2 topic list 2>/dev/null | grep -c '^/dmp/frames$')" "0"
chk "unconfigured 时自研服务不在 service list" \
    "$(timeout 15 ros2 service list 2>/dev/null | grep -c '^/dmp/bed01/selftest$')" "0"
echo "  (为什么不用 ros2 service call 的报错文本来断言: 服务不存在时它会一直卡在 waiting for service,"
echo "   timeout 剔掉后 stdout 是空的, 只会得到一句[到底是没起来还是没联通]的模糊结论 —— 见素材录 B-15)"
echo "  (生命周期服务 change_state 反过来必须一直在 —— 它是进入其它状态的唯一入口, 由 LifecycleNode 基类在构造期创建)"
chk "change_state 服务在" "$(timeout 15 ros2 service list 2>/dev/null | grep -c "/$NODE/change_state")" "1"

echo "=== [2] configure -> inactive: 话题/服务出现, 但零数据, 且写路径被拒 ==="
timeout 30 ros2 lifecycle set /$NODE configure 2>&1 | tail -1
sleep 2
chk "state=inactive" "$(timeout 15 ros2 lifecycle get /$NODE 2>/dev/null | awk '{print $1}')" "inactive"
chk "configure 后 /dmp/frames 出现" "$(timeout 15 ros2 topic list 2>/dev/null | grep -c '^/dmp/frames$')" "1"
recorder 6 "$LOG/gap_inactive.json"
echo "  inactive 期间采样: $(cat "$LOG/gap_inactive.json")"
chk "inactive 零数据(守卫生效, 不是碰巧)" "$(python3 -c "import json;print(sum(json.load(open('$LOG/gap_inactive.json'))['count'].values()))")" "0"
echo "  inactive 下 selftest(应当仍在并自报状态):"
timeout 20 ros2 service call /dmp/bed01/selftest dmp_msgs/srv/Selftest "{probe: while-inactive}" 2>&1 | tr -d '\n' \
  | grep -o "healthy=[A-Za-z]*\|state=inactive" | sort -u | sed 's/^/    /'
timeout 20 ros2 service call /dmp/bed01/set_rule dmp_msgs/srv/SetRule "{kind: 2, low: 45.0, high: 115.0}" 2>&1 \
  | grep -o "accepted=[A-Za-z]*\|reason='[^']*'" | sed 's/^/  set_rule 未激活: /'
echo "  get_rules 未激活仍可读(读操作不该被状态机挡住): $(timeout 20 ros2 service call /dmp/bed01/get_rules dmp_msgs/srv/GetRules '{}' 2>&1 | grep -c 'Rule(' ) 条 Rule("

echo "=== [3] activate -> active: 数据与写路径同时恢复 ==="
timeout 30 ros2 lifecycle set /$NODE activate 2>&1 | tail -1
sleep 2
chk "state=active" "$(timeout 15 ros2 lifecycle get /$NODE 2>/dev/null | awk '{print $1}')" "active"
recorder 6 "$LOG/gap_active.json"
echo "  active 期间采样: $(cat "$LOG/gap_active.json")"
ACTIVE_N=$(python3 -c "import json;print(sum(json.load(open('$LOG/gap_active.json'))['count'].values()))")
[ "$ACTIVE_N" -gt 20 ] && { echo "  PASS active 有数据 (=$ACTIVE_N 批)"; PASS=$((PASS+1)); } \
                       || { echo "  FAIL active 只有 $ACTIVE_N 批"; FAIL=$((FAIL+1)); }
timeout 20 ros2 service call /dmp/bed01/set_rule dmp_msgs/srv/SetRule \
  "{kind: 2, low: 45.0, high: 115.0, message: 'active-test'}" 2>&1 | grep -o "accepted=[A-Za-z]*" | sed 's/^/  set_rule 激活后: /'

echo "=== [4] deactivate -> cleanup: 数据再次归零, 且 publisher 真的被释放 ==="
timeout 30 ros2 lifecycle set /$NODE deactivate 2>&1 | tail -1
sleep 2
recorder 5 "$LOG/gap_deact.json"
DEACT_N=$(python3 -c "import json;print(sum(json.load(open('$LOG/gap_deact.json'))['count'].values()))")
chk "deactivate 后再次零数据(证明状态机不是只做一次性启动)" "$DEACT_N" "0"
timeout 30 ros2 lifecycle set /$NODE cleanup 2>&1 | tail -1
sleep 2
chk "cleanup 后 /dmp/frames 再消失(on_cleanup 真释放了 publisher)" \
    "$(timeout 15 ros2 topic list 2>/dev/null | grep -c '^/dmp/frames$')" "0"
echo "  (只打一行日志不算证据: 话题从 ROS 图上消失才是 publisher 对象析构的外部可观测结果)"
kill "$LAUNCH" 2>/dev/null; pkill -f bridge_node 2>/dev/null; sleep 2

echo "=== [5] 组合容器 A/B: 一台设备的阻塞 RPC 会不会拖住另一台设备的数据面 ==="
# 方法: 同容器跑 bed01+bed02, 对 bed01 连续打 set_rule(服务端要做一次同步的网关 RULES 多行回读),
#       同时只测 bed02 的 /dmp/frames 到达间隔。
# 为什么分两个进程: 如果把"打服务"和"记到达时间"放进同一个 rclpy 单线程执行器,
# 同步调用本身就挡住了订阅回调 —— 那测出来的是取证工具的阻塞, 不是被试系统的。
run_ab() {  # run_ab <true=mt容器|false=st容器>
  local mt=$1 tag=$2
  pkill -f component_container 2>/dev/null; sleep 2
  nohup ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py \
        devices:=bed01,bed02 mt:="$mt" gateway_port:="$GW_PORT" > "$LOG/compose_$tag.log" 2>&1 &
  local lp=$!
  sleep 10
  local d
  for d in bed01 bed02; do
    timeout 30 ros2 lifecycle set /dmp_bridge_$d configure >/dev/null 2>&1
    timeout 30 ros2 lifecycle set /dmp_bridge_$d activate  >/dev/null 2>&1
  done
  sleep 3
  recorder 12 "$LOG/gap_${tag}_quiet.json" > /dev/null 2>&1     # 空载基线
  nohup timeout -s INT 12 python3 - "/dmp/bed01/set_rule" 10 > "$LOG/hammer_$tag.log" 2>&1 <<'PY' &
import sys, time
import rclpy
from rclpy.node import Node
from dmp_msgs.srv import SetRule
svc, dur = sys.argv[1], float(sys.argv[2])
rclpy.init(); n = Node('dmp_ctl_hammer')
cl = n.create_client(SetRule, svc)
if not cl.wait_for_service(timeout_sec=10):
    print('NO_SERVICE'); sys.exit(1)
t0 = time.monotonic(); i = 0; lat = []
while time.monotonic() - t0 < dur:
    i += 1
    req = SetRule.Request(); req.kind = 2; req.low = 45.0 + (i % 5); req.high = 115.0
    req.message = 'hammer'
    t1 = time.monotonic()
    fut = cl.call_async(req)
    rclpy.spin_until_future_complete(n, fut, timeout_sec=5)
    lat.append((time.monotonic() - t1) * 1000.0)
print('calls=%d ctl_avg_ms=%.1f ctl_max_ms=%.1f' % (i, sum(lat)/max(1,len(lat)), max(lat) if lat else 0.0))
PY
  HAMMER=$!
  sleep 2      # 给 hammer 一点发现服务的时间, 否则前面两秒可能在空等, 阻塞没落在采样窗口里
  recorder 10 "$LOG/gap_${tag}_busy.json" > /dev/null 2>&1
  wait "$HAMMER" 2>/dev/null
  kill "$lp" 2>/dev/null; pkill -f component_container 2>/dev/null; sleep 2
}
run_ab true mt
run_ab false st

show() {  # show <tag> —— 打印 bed02 在空载/被阻塞两种情形下的最大间隔
  python3 - "$1" "$LOG" <<'PY'
import json, sys
tag, d = sys.argv[1], sys.argv[2]
def g(f):
    try:
        j = json.load(open(f"{d}/gap_{tag}_{f}.json"))
        return j['count'].get('bed02', 0), j['max_gap_ms'].get('bed02', 0.0)
    except Exception:
        return 0, 0.0
def h():
    try:
        return open(f"{d}/hammer_{tag}.log").read().strip()
    except Exception:
        return '(hammer 日志缺失)'
qn, qg = g('quiet'); bn, bg = g('busy')
print(f"  {tag:3s}: 空载 批数={qn:3d} 最大间隔={qg:6.1f}ms | 控制面打满 批数={bn:3d} 最大间隔={bg:6.1f}ms")
print(f"        bed01 一次 set_rule 的服务端耗时: {h()}")
PY
}
echo "--- mt (component_container_mt, 两个回调组分到不同线程) ---"; show mt
echo "--- st (component_container, 单线程: 一个回调阻塞 = 全容器阻塞) ---"; show st
echo "  判读口径: st 的 busy 最大间隔应显著大于其 quiet 基线(≈batch_period_ms=50), 而 mt 两个数字接近。"
echo "  若 st≈mt, 说明本轮实验没造出足够长的阻塞 —— 是实验失败, 不要读成[多线程没用]。"

echo "=== [6] 汇总 ==="
echo "  PASS=$PASS FAIL=$FAIL   (Q3 的状态机断言全部走这几个 PASS/FAIL, 不靠人眼看日志)"
echo "  Q4 的结论不在这里: 它是一组毫秒数, 看上面 mt / st 两行自己比。"
pkill -f component_container 2>/dev/null; pkill -f bridge_node 2>/dev/null
kill "$GW" "$SIM" 2>/dev/null
[ "$FAIL" -eq 0 ] && echo "ALL_LIFECYCLE_ASSERTS_PASS"
echo "SCRIPT_DONE"
