#!/usr/bin/env bash
# vm_lifecycle_compose.sh — 生命周期升级(c8)与"长回调"(c13~c16)的验收
#
#   Q3: "不激活就绝不采集" 是**契约**还是**愿望**?  -> [1]~[4] 用负例把它钉死
#   Q4: 两个回调组 + 多线程容器 到底买到了什么?    -> [5] 用毫秒数回答 (结论: 没买到)
#   Q6: 那什么才能让它真输掉? -> [6] 把一台设备的网关端点换成坏地址, 造一个**单个长回调**
#   Q7: 一个组件 configure 失败, 同容器的另一台还活着吗? -> [7] 错误隔离格
#
# 用法(VM 内): bash tools/vm_lifecycle_compose.sh
#   POISON_LEVELS="mid" 只跑某几档毒化; POISON=0 完全跳过 [6][7] (只要那 10 条状态机断言时快得多)。
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

# 图上的"必须不存在"断言不能走 ros2 CLI 的默认路径: 那个读的是 ros2-daemon 的图缓存,
# 而缓存会活得比参与者久(探针实测: 一个没来得及道别就消失的发布者, daemon 路报 1 /
# 现场路报 0 / 停掉 daemon 后两路同时归零)。拿缓存做负断言, 错的方向恰好是"假红"。
# 现场发现 = --no-daemon + 给足 spin-time; spin-time 不能小, 否则正断言会因为发现没走完而假红,
# 那就是用一个新坑换掉一个旧坑。
SPIN=6
g_topics()  { timeout 30 ros2 topic list   --no-daemon --spin-time $SPIN 2>/dev/null; }
g_services(){ timeout 30 ros2 service list --no-daemon --spin-time $SPIN 2>/dev/null; }
g_nodes()   { timeout 30 ros2 node list    --no-daemon --spin-time $SPIN 2>/dev/null; }
# 两路读数都打出来: 它们不一致本身就是读数, 说明本轮有一次退出没道别、缓存在说谎。
# 存活名单必须包含 recorder/python 订阅方: 纯订阅方也会让话题出现在 list 里,
# 而上一版的 pgrep 只看容器与 bridge_node —— 真凶要是个订阅者, 这个诊断本身就分辨不开。
diag_graph() {  # diag_graph <标签>
  echo "  [图诊断 $1] /dmp/frames: 现场=$(g_topics | grep -c '^/dmp/frames$') daemon=$(timeout 20 ros2 topic list 2>/dev/null | grep -c '^/dmp/frames$')"
  echo "  [图诊断 $1] 容器/桥: $(pgrep -af 'component_container|bridge_node' 2>/dev/null | tr '\n' ';' | cut -c1-160)"
  echo "  [图诊断 $1] 其它 ROS 参与者(python 订阅方/CLI): $(pgrep -af 'python3 - |ros2 topic|ros2 service|ros2 lifecycle' 2>/dev/null | grep -v pgrep | tr '\n' ';' | cut -c1-160)"
  echo "  [图诊断 $1] 上一个容器已活多久: $(ps -eo etimes=,args= 2>/dev/null | grep -E 'component_container|bridge_node' | grep -v grep | awk '{print $1}' | tr '\n' ' ')"
}

# 记录 /dmp/frames 各来源(frame_id)的到达间隔。订阅 QoS 必须 best_effort:
# 桥默认发 sensor(best_effort), 用 reliable 订阅会得到"零数据", 那测的是 QoS 不是生命周期。
recorder() {  # recorder <秒> <输出json文件> [停顿阈值ms=400]
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
r = lambda d: {k: round(v, 1) if isinstance(v, float) else v for k, v in d.items()}
print(json.dumps({'count': cnt, 'max_gap_ms': r(gap), 'stalls': r(stall), 'thr_ms': thr}))
PY
}

# hammer <日志> <秒> <并发窗口>：故意**不**等上一个回复就发下一个。
# 为什么必须成批 in-flight: 一次 set_rule 只阻 86ms, 而 bed02 帧的自然节律≈200ms ——
# 单发的阻塞低于噪声底, st/mt 两列会几乎相等, 看着像"多线程没用", 实际是实验没造出现象。
# 窗口 K=8 是把一次 86ms 拼成一条 ~700ms 的持续队列, 才能跨过 200ms 的自然节律。
# 结果(诚实起见写完再跑, 且下面是 7 轮重复后的口径, 不是单轮): 队列确实排起来了(往返均值 650ms),
# 但**数据面几乎不动** —— mt/st 各自"打满 − 空载"的最大间隔差值**符号在轮次间来回翻转**(各 4 正 3 负),
# 中位数 mt +31.2ms / st +15.5ms; 14 个采样窗口里只出现 1 次 >400ms 停顿(474.2ms),
# 而且落在**应当免疫的 mt 臂**。所以本轮结论是"头阻塞假设在此负载下不成立",
# 数字与降级后的说法见 docs/adr/ADR-003。不要拿本脚本当"多线程有用"的证据。
# 三条测量学教训(都是跑第 5、6、7 轮才暴露的, 详见素材录 B-35/B-36):
#   1) 单轮不能归因: 一次 474ms 停顿既可能是调度也可能是 DDS/订阅端抖动, 判据是**重复 >=5 轮看符号分布**;
#   2) 批数本身在 54~60 之间抖, 所以"掉了 5 批"不构成证据 —— 噪声底≈10%, 任何小于它的差都不要读数;
#   3) 中位数的**大小关系**也随样本集合翻转(6 轮时 mt < st, 7 轮时 mt > st) —— 能翻转的排序就不是效应。
hammer() {
  local out=$1 dur=$2 kwin=$3
  # 外层 timeout 必须 > 主循环 dur + 最坏排空时间 + 余量: 只给 dur (或恰好等于 dur+排空期限)
  # 会在最后一轮调用/排空里被 SIGINT 打断, print 来不及执行 -> 日志里只剩一段 KeyboardInterrupt
  # traceback, 数字全丢 (K=8 与 K=32 都真实发生过: 内层排空期限是 6s, 外层给 dur+8 就是刀尖重合)。
  timeout -s INT $(( dur + 16 )) python3 - "$dur" "$kwin" > "$out" 2>&1 <<'PY'
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
        if f.done():
            lat.append((time.monotonic() - t1) * 1000.0)
        else:
            keep.append((f, t1))
    del inflight[:]
    inflight.extend(keep)
while time.monotonic() - t0 < dur:
    while len(inflight) < kwin:
        req = SetRule.Request(); req.kind = 2
        req.low = 45.0 + (sent % 5); req.high = 115.0; req.message = 'hammer'
        inflight.append((cl.call_async(req), time.monotonic())); sent += 1
    rclpy.spin_once(n, timeout_sec=0.02); reap()
    if len(inflight) > peak: peak = len(inflight)
deadline = time.monotonic() + 6
while inflight and time.monotonic() < deadline:
    rclpy.spin_once(n, timeout_sec=0.05); reap()
print('calls=%d peak_inflight=%d ctl_avg_ms=%.1f ctl_max_ms=%.1f  (客户端观测往返, 含排队)' % (
    len(lat), peak, sum(lat)/max(1, len(lat)), max(lat) if lat else 0.0))
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
diag_graph "[1]开头"
echo "  node 在吗: $(g_nodes | grep -c "/$NODE")  (期望 1: 节点本身构造即存在)"
chk "unconfigured 时 /dmp/frames 不在 topic list" \
    "$(g_topics | grep -c '^/dmp/frames$')" "0"
chk "unconfigured 时自研服务不在 service list" \
    "$(g_services | grep -c '^/dmp/bed01/selftest$')" "0"
echo "  (为什么不用 ros2 service call 的报错文本来断言: 服务不存在时它会一直卡在 waiting for service,"
echo "   timeout 剔掉后 stdout 是空的, 只会得到一句[到底是没起来还是没联通]的模糊结论 —— 见素材录 B-15)"
echo "  (生命周期服务 change_state 反过来必须一直在 —— 它是进入其它状态的唯一入口, 由 LifecycleNode 基类在构造期创建)"
chk "change_state 服务在" "$(g_services | grep -c "/$NODE/change_state")" "1"

echo "=== [2] configure -> inactive: 话题/服务出现, 但零数据, 且写路径被拒 ==="
timeout 30 ros2 lifecycle set /$NODE configure 2>&1 | tail -1
sleep 2
chk "state=inactive" "$(timeout 15 ros2 lifecycle get /$NODE 2>/dev/null | awk '{print $1}')" "inactive"
chk "configure 后 /dmp/frames 出现" "$(g_topics | grep -c '^/dmp/frames$')" "1"
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
diag_graph "[4]cleanup后"
chk "cleanup 后 /dmp/frames 再消失(on_cleanup 真释放了 publisher)" \
    "$(g_topics | grep -c '^/dmp/frames$')" "0"
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
  recorder 12 "$LOG/gap_${tag}_quiet.json" 400 > /dev/null 2>&1   # 空载基线 (停顿阈值 400ms)
  hammer "$LOG/hammer_$tag.log" 16 8 &
  HAMMER=$!
  sleep 2      # 给 hammer 一点发现服务的时间, 否则前面两秒可能在空等, 阻塞没落在采样窗口里
  recorder 12 "$LOG/gap_${tag}_busy.json" 400 > /dev/null 2>&1
  wait "$HAMMER" 2>/dev/null
  kill "$lp" 2>/dev/null; pkill -f component_container 2>/dev/null; sleep 2
}
run_ab true mt
run_ab false st

show() {  # show <tag> —— 打印 bed02 在空载/被阻塞两种情形下的最大间隔
  python3 - "$1" "$LOG" <<'PY'
import json, re, sys
tag, d = sys.argv[1], sys.argv[2]
def g(f):
    try:
        j = json.load(open(f"{d}/gap_{tag}_{f}.json"))
        return (j['count'].get('bed02', 0), j['max_gap_ms'].get('bed02', 0.0),
                j.get('stalls', {}).get('bed02', 0))
    except Exception:
        return 0, 0.0, 0
def h():
    try:
        return open(f"{d}/hammer_{tag}.log").read().strip()
    except Exception:
        return '(hammer 日志缺失)'
qn, qg, qs = g('quiet'); bn, bg, bs = g('busy')
hh = h()
print(f"  {tag:3s}: 空载 批数={qn:3d} 最大间隔={qg:6.1f}ms 停顿(>400ms)={qs:2d} | "
      f"控制面打满 批数={bn:3d} 最大间隔={bg:6.1f}ms 停顿(>400ms)={bs:2d}")
print(f"        bed01 的 set_rule 队列: {hh}")
# 两个判据必须分开, 否则会把"实验没做够"与"假设不成立"混成同一句:
#   队列没排起来   -> [实验无效]   , 什么结论都不能下 (负载本身没造出来);
#   排起来了而 st 数据面未拖住 -> [假设不成立], 这是有效负结果。
# 阈值: 单请求本身≈86ms, 往返均值>200ms 才说明有排队; 峰值并发>=K/2 才说明窗口真填满了。
m = re.search(r'peak_inflight=(\d+).*?ctl_avg_ms=([0-9.]+)', hh)
load_ok = bool(m) and float(m.group(2)) > 200.0 and int(m.group(1)) >= 4
if not load_ok:
    print("        [实验无效] 控制面没排起队列 (往返均值或峰值并发过低) —— 两个结论都不能下")
elif tag == 'st' and bg < 2.0 * max(qg, 1.0):
    print("        [假设不成立] 队列已排起(往返 %s ms, 峰值并发 %s)但 st 数据面未被拖住"
          % (m.group(2), m.group(1)))
    print("                   → 损害被单个回调时长封顶, 不随队列总长放大; 不要读成[实验失败]")
PY
}
echo "--- mt (component_container_mt, 两个回调组分到不同线程) ---"; show mt
echo "--- st (component_container, 单线程: 一个回调阻塞 = 全容器阻塞) ---"; show st
echo "  判读口径: 帧的自然节律≈200ms(仿真器 5Hz), 所以基线不是 batch_period_ms=50 —— 停顿阈值取 400ms。"
echo "  预设预期(写完再跑): st 的 busy 最大间隔/停顿次数显著高于其 quiet 基线, mt 两行接近。"
# ↓ 这一行必须**从本轮 json 现算**, 不能写成固定句子。写死过一次, 后果是: 第 5 轮 mt 臂出现了
# 474ms 停顿(方向与假设相反), 脚本却照旧打印"假设不成立" —— 数据反对它的时候它还在替结论说话。
python3 - "$LOG" <<'PY'
import json, sys
d = sys.argv[1]
def g(tag, f):
    try:
        j = json.load(open(f"{d}/gap_{tag}_{f}.json"))
        return (j['count'].get('bed02', 0), j['max_gap_ms'].get('bed02', 0.0),
                j.get('stalls', {}).get('bed02', 0))
    except Exception:
        return 0, 0.0, 0
part = []
for tag in ('mt', 'st'):
    qn, qg, qs = g(tag, 'quiet'); bn, bg, bs = g(tag, 'busy')
    part.append(f"{tag}: 空载{qg:.1f}→打满{bg:.1f}ms(差{bg-qg:+.1f}) 批数{qn}→{bn} 停顿{qs}→{bs}")
print("  本轮实测: " + " | ".join(part))
print("  本轮差值的符号/大小都不足以归因(单轮); mt/st 孰优孰劣的结论只在 ADR-003 的 7 轮重复表里, "
      "而那张表里连中位数的大小关系都会随样本集合翻转 —— 那就是没有效应。")
PY

echo "=== [6] 毒化 A/B: 把一台设备的网关端点换成坏地址, 单线程容器会不会真的输 ==="
# [5] 的负结果留下一句未闭环的话: "损害被单个回调时长封顶, 不随队列总长放大"。
# 那就把**单个回调的时长**拉到 3s / 21s 去看封顶到底存不存在 —— 这是 ADR-003 里唯一未覆盖的
# 边界, 也是目前唯一能让 st 真正输掉、能给多回调组找回*性能*理由的场景。
# 长回调不需要人造: connectNew() 以前没有超时, 而它跑在**调用方的回调里**
# (tickLink 定时器 / 三个服务 / 连 on_configure 的 refreshRules 都走它),
# 所以"一次网络故障 = 整个容器冻结那么久"本来就已经是产品行为。
# 三档毒化(全部**现场量**, 不写死数字 —— 写死过一次, 后果见素材录 B-35):
#   fast : 本机无监听端口     -> 立即 ECONNREFUSED, 对照组: 证明"毒化"这个动作本身不伤数据面
#   mid  : 同网段不存在的主机 -> 内核 SYN 重试到 EHOSTUNREACH (2026-09-29 VM 实测≈3.1s)
#   worst: 私网黑洞(包被 drop) -> 重试到超时而失败   (同日实测≈21s)
POISON="${POISON:-1}"
POISON_LEVELS="${POISON_LEVELS:-fast mid worst}"
: > "$LOG/poison_result.txt"; : > "$LOG/poison_cells.txt"

poison_probe() {  # poison_probe <host> <port> -> "elapsed_ms=<n> outcome=<err>"; 不设 socket 超时,
                  # 量的就是内核默认的 SYN 重试代价(那正是要封顶的东西), 所以只能不超时
  timeout 60 python3 - "$1" "$2" <<'PY' 2>/dev/null
import errno, socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
s = socket.socket(); s.setblocking(True); t0 = time.monotonic(); out = 'connected'
try:
    s.connect((host, port))
except OSError as e:
    out = 'err=%s' % errno.errorcode.get(e.errno, e.errno)
print('elapsed_ms=%.0f outcome=%s' % ((time.monotonic() - t0) * 1000.0, out))
s.close()
PY
}

pick_poison() {  # pick_poison <名字> <期望区间 lo,hi(ms)> <候选 host:port 列表> -> 全局 POISON_<名字> / MS_<名字>
  local name=$1 band=$2 cands=$3 lo hi c el oc
  lo=$(echo "$band" | cut -d, -f1); hi=$(echo "$band" | cut -d, -f2)
  # bash 里 `POISON_$name=''` 不成立: 赋值号左边的名字不能靠展开构造, 整串会被当命令执行
  # (现象是两句 command not found, 而变量**没被清空** —— 上一档选中的值会漏到下一档)。
  eval "POISON_$name=''; MS_$name=0"
  for c in $cands; do
    read -r el oc <<< "$(poison_probe "${c%%:*}" "${c##*:}")"
    el=${el#elapsed_ms=}; [ -z "$el" ] && el=0
    echo "    候选 $c: $oc $el ms"
    if [ "$(python3 -c "print(1 if $lo <= float('$el') <= $hi else 0)")" = "1" ]; then
      eval "POISON_$name=$c"; eval "MS_$name=$el"; return 0
    fi
  done
  return 1
}

# 黑洞地址是**跟网段走的**, 不能写死: 换一台宿主机/换一种网络模式就失效, 而失效的表现是
# "什么都没毒到" —— 那会被读成[毒化没影响]。所以按本机路由反推子网, 再按**实测时长**归档。
SRC_IP=$(ip -4 route get 1.1.1.1 2>/dev/null | awk '{for(i=1;i<NF;i++) if($i=="src"){print $(i+1); exit}}')
SUBNET=${SRC_IP%.*}
GATEWAY_IP=$(ip -4 route 2>/dev/null | awk '$1=="default"{print $3; exit}')
SLOW_CANDS="${SUBNET}.253:9100 ${SUBNET}.244:9100 ${SUBNET}.201:9100 ${SUBNET}.99:9100"
WORST_CANDS="10.255.255.1:9100 198.51.100.1:9100"
FAST_EP="127.0.0.1:19999"

if [ "$POISON" != "1" ]; then
  echo "  POISON=$POISON -> 跳过 [6][7] (只看状态机断言)"
elif pick_poison mid 1000,8000 "$SLOW_CANDS"; then
  echo "  mid  档 = $POISON_mid ($(printf '%.0f' "$MS_mid") ms)"
else
  echo "  [实验无效] 没找到 1~8s 档的毒化目标(候选里没一个落在区间内) —— mid 的四格全部不跑"
fi
if pick_poison worst 8000,60000 "$WORST_CANDS"; then
  echo "  worst档 = $POISON_worst ($(printf '%.0f' "$MS_worst") ms)"
else
  echo "  (没有 >8s 的黑洞目标, worst 档不跑 —— 这不阻断实验, mid 档才是主结论)"
fi
read -r FAST_MS _ <<< "$(poison_probe 127.0.0.1 19999)"; FAST_MS=${FAST_MS#elapsed_ms=}
echo "  fast 档 = $FAST_EP (${FAST_MS:-?} ms, 对照组)"

# 采样窗口长度由**实测阻塞时长**算, 因为固定 14s 遇到 21s 停顿只会得到"0 批",
# 而"0 批"与"容器没起来""QoS 不匹配"是同一个可观测现象 —— 分不开就是假结论。
# (但跑到本轮才知道这不是全部: 真正被毒死时占空比接近 100%, 46s 窗口里也只有 1 批,
#  而**只有 1 个样本就算不出间隔**。所以下面的表把"批数速率"当主统计量, gap 当辅助。)
poison_dur() { python3 -c "print(max(20, min(75, int(${1:-0}/1000) + 25)))"; }

add_cell() {  # add_cell <level> <tag> <mt> <conn_timeout_ms> <endpoint> <实测时长ms>
  case " $POISON_LEVELS " in *" $1 "*) ;; *) return 0;; esac
  # 守的是**端点**(第 5 个), 不是容器类型: 目标没找到时端点是空的, 这时必须整格不跑。
  # 跑一个空端点会得到"毒化没影响"的读数, 而那其实是"什么都没毒到"。
  [ -z "$5" ] && return 0
  printf '%s|%s|%s|%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" "$5" "$(poison_dur "$6")" "${6:-0}" \
    >> "$LOG/poison_cells.txt"
}

poison_cell() {  # poison_cell <tag> <mt> <ct> <ep> <level> <dur秒> <阻塞秒>
  local tag=$1 mt=$2 ct=$3 ep=$4 level=$5 dur=$6
  local bs=${7:-2}; [ "$bs" -lt 2 ] && bs=2
  # 生命周期命令的预算必须跟着**阻塞时长**缩放, 不能写死 20/40/90:
  # 首跑就是被自己绊了一跤 —— 单线程容器被 21s 的回调占满时, 连
  # `timeout 20 ros2 lifecycle get` 都无人应答, 于是状态被记成 unknown,
  # 看起来像"毒化没跑", 实际恰恰是"毒得连问一句话都排不上队"。
  local tcfg=$(( 30 + 3 * bs )) tact=$(( 30 + 2 * bs )) tq=$(( 15 + bs ))
  pkill -f component_container 2>/dev/null; sleep 2
  local args="devices:=bed01,bed02 mt:=$mt gateway_port:=$GW_PORT connect_timeout_ms:=$ct"
  [ "$ep" != "none" ] && args="$args gw_overrides:=bed01=$ep"
  # shellcheck disable=SC2086
  nohup ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py $args > "$LOG/poison_$tag.log" 2>&1 &
  local lp=$!
  sleep 10
  timeout 40 ros2 lifecycle set /dmp_bridge_bed02 configure >/dev/null 2>&1
  timeout 40 ros2 lifecycle set /dmp_bridge_bed02 activate  >/dev/null 2>&1
  sleep 2
  recorder 8 "$LOG/pgap_${tag}_base.json" 400 >/dev/null 2>&1     # 配对基线: 同容器, bed01 还没激活
  # 上面那句是本档能不能下结论的**前提**: 没有基线就没有差值, 只有基线才谈得上"被拖住"。
  timeout "$tcfg" ros2 lifecycle set /dmp_bridge_bed01 configure >/dev/null 2>&1
  timeout "$tact" ros2 lifecycle set /dmp_bridge_bed01 activate  >/dev/null 2>&1
  recorder "$dur" "$LOG/pgap_${tag}_poison.json" 400 >/dev/null 2>&1
  local b1 st tmo_raw tmo_max
  b1=$(timeout "$tq" ros2 lifecycle get /dmp_bridge_bed01 2>/dev/null | awk '{print $1}')
  # 计数只取 bed01 那一路的日志: 两路共用同一条 WARN 文案, 不过滤就会把别人的 0 当成我的 0。
  tmo_raw=$(grep 'dmp_bridge_bed01' "$LOG/poison_$tag.log" 2>/dev/null | grep -c 'connect 超时累计')
  tmo_max=$(grep 'dmp_bridge_bed01' "$LOG/poison_$tag.log" 2>/dev/null \
            | grep -o 'connect 超时累计 [0-9]*' | grep -o '[0-9]*$' | sort -n | tail -1)
  # 状态读不到时记成 probe_timeout 而不是 unknown: 它本身就是一条读数(容器被占到无法应答查询),
  # 与"读了但不对"必须可区分 —— 否则下一轮又要把两者当同一件事查。
  st="${b1:-probe_timeout}"
  echo "$tag|$mt|$ct|$ep|$level|$st|${tmo_raw:-0}|${tmo_max:-0}|8|$dur" >> "$LOG/poison_result.txt"
  kill "$lp" 2>/dev/null; pkill -f component_container 2>/dev/null; sleep 2
}

if [ "$POISON" = "1" ]; then
  add_cell mid  mid_st_old  false 0    "$POISON_mid"   "${MS_mid:-0}"
  add_cell mid  mid_mt_old  true  0    "$POISON_mid"   "${MS_mid:-0}"
  add_cell mid  mid_st_new  false 1000 "$POISON_mid"   "${MS_mid:-0}"
  add_cell mid  mid_mt_new  true  1000 "$POISON_mid"   "${MS_mid:-0}"
  add_cell fast fast_st_old false 0    "$FAST_EP"      0
  add_cell fast fast_mt_new true  1000 "$FAST_EP"      0
  add_cell worst worst_st_old false 0   "$POISON_worst" "${MS_worst:-0}"
  add_cell worst worst_st_new false 1000 "$POISON_worst" "${MS_worst:-0}"
  while IFS='|' read -r level tag mt ct ep dur ems; do
    echo "  --- 格 $tag (容器=$mt, connect_timeout_ms=$ct, 毒化=$ep, 阻塞≈$(( ems / 1000 ))s, 窗口=${dur}s) ---"
    poison_cell "$tag" "$mt" "$ct" "$ep" "$level" "$dur" "$(( ems / 1000 ))"
  done < "$LOG/poison_cells.txt"

  python3 - "$LOG" <<'PY'
import json, sys
D = sys.argv[1]
cells = {}
for ln in open(f"{D}/poison_result.txt"):
    p = ln.strip().split('|')
    if len(p) == 8:
        cells[p[0]] = dict(zip(('tag','mt','ct','ep','level','state','tmo_raw','tmo'), p))
    elif len(p) == 10:
        d = dict(zip(('tag','mt','ct','ep','level','state','tmo_raw','tmo','bdur','pdur'), p))
        cells[p[0]] = d
def g(tag, which, key, dflt):
    try:
        j = json.load(open(f"{D}/pgap_{tag}_{which}.json"))
        return j[key].get('bed02', dflt)
    except Exception:
        return dflt
print("  每格只报本轮读数(bed02 侧); 基线与受毒是**同一次运行内的配对**, 配对差之外不可比(B-36):")
print("  主统计量是**批数速率**(它永远有定义); 最大间隔只当辅助 ——")
print("  它至少要两个样本才算得出来, 而真被毒死时整个窗口只到 1 批, 那就是无定义。")
print("  无定义时旧版会把 0.0ms 当成一个数打印, 读起来就像“毫无影响” —— 那是比写死结论更阴的错。")
print("  %-13s %-9s %-6s %-6s %9s %9s %7s %9s %6s" % (
    "格", "容器", "限时", "档位", "基线gap", "受毒gap", "速率比", "批/s→", "超时次"))
for c in cells.values():
    q = g(c['tag'], 'base', 'max_gap_ms', 0.0); b = g(c['tag'], 'poison', 'max_gap_ms', 0.0)
    qn = g(c['tag'], 'base', 'count', 0); bn = g(c['tag'], 'poison', 'count', 0)
    # 速率用两个窗口各自的实际秒数归一(它们不等长!), 直接比批数会把"28s 窗口"当成"46s 窗口"。
    bd = float(c.get('bdur') or 8); pd = float(c.get('pdur') or 25)
    qrate, prate = qn / bd, bn / pd
    ratio = (qrate / prate) if prate > 0 else float('inf')
    # gap 只在两边都算得出时才有意义; 受毒侧样本不足时明写, 不留一个看似正常的 0.0。
    btxt = ('%9.1f' % b) if bn >= 2 else '  样本不足'
    rtxt = ('%7.1fx' % ratio) if ratio != float('inf') else '   全饿死'
    warn = ''
    if c['state'] == 'probe_timeout':
        warn = '  [占到无法应答查询] 连状态都读不到 —— 这本身就是本格的读数'
    elif c['state'] != 'active':
        warn = '  [实验无效] bed01 未激活(%s), 毒化根本没跑' % c['state']
    elif qn < 10:
        warn = '  [实验无效] 基线就只有 %d 批, 容器/订阅不健康' % qn
    print("  %-13s %-9s %-6s %-6s %9.1f %s %s %5.2f->%-5.2f %6s%s" % (
        c['tag'], c['mt'], c['ct'], c['level'], q, btxt, rtxt, qrate, prate, c['tmo'], warn))
# 本轮能不能下结论, 由上面的数据判, 不由这段脚本里写死的句子判。
mid = {t: c for t, c in cells.items() if c['level'] == 'mid' and c['state'] == 'active'}
if {'mid_st_old', 'mid_mt_old'} <= set(mid):
    print("  两格都有配对基线, mid 档可以下结论(数字见上表, 归因需要重复轮次 —— 见 ADR-003 的符号分布做法)。")
else:
    print("  mid 档格数不足或 bed01 未激活, 本轮**不下结论**。")
PY

  # 下面四条是**契约**断言(判据可以写死, 结论文案不行)。它们检验的是"限时有没有挂在活路径上",
  # 以及"对照组是不是真的没被新代码改行为" —— 后者不成立的话, 上面整张表都是在比两次新实现。
  BAD_NEW=$(awk -F'|' '$3+0>0 && $5=="mid" && $6=="active" && $7+0==0' "$LOG/poison_result.txt" | wc -l)
  chk "限时档(ct>0)的日志里确有'connect 超时累计'(修复挂在活路径上) 失格=$BAD_NEW" "$BAD_NEW" "0"
  BAD_OLD=$(awk -F'|' '$3+0==0 && $5=="mid" && $8+0>0' "$LOG/poison_result.txt" | wc -l)
  chk "不限时档(ct=0)的超时计数必须为 0(对照组没被污染) 失格=$BAD_OLD" "$BAD_OLD" "0"
  NOTACT=$(awk -F'|' '$5!="" && $6!="active" && $6!="probe_timeout"' "$LOG/poison_result.txt" | wc -l)
  # probe_timeout **不算失格**: 那是"容器被占到连一句查询都无人应答"的读数, 是现象不是事故。
  # 但 unconfigured/inactive 必须算失格 —— 那才是"毒化根本没跑", 整张表都是空的。
  chk "每一格的 bed01 要么真激活了, 要么被占到读不到状态 失格=$NOTACT" "$NOTACT" "0"
  NOBASE=$(awk -F'|' '{print $1}' "$LOG/poison_result.txt" | while read -r t; do
             python3 -c "import json;print(0 if sum(json.load(open('$LOG/pgap_${t}_base.json'))['count'].values())>=10 else 1)" 2>/dev/null
           done | awk '{s+=$1} END{print s+0}')
  chk "每一格都取到配对基线 失格=$NOBASE" "$NOBASE" "0"
fi

echo "=== [7] 错误隔离: 一个组件 configure 失败, 同容器的另一台还活着吗 ==="
# 组合把 N 台设备塞进一个进程的代价就在这里: 一次 FAILURE 是只毁这台, 还是毁整个容器?
# 靠什么造"只有 bed01 失败"而不改桥的代码: gw_overrides 里把 bed01 的端口写成 0 ——
# launch 侧的 _parse_overrides 允许 0(它是数字), 而 C++ 的 on_configure 明确拒掉 port=0。
# 于是这是一条**真**的每设备失败路径, 不是我为了测试新加的钩子。
if [ "$POISON" = "1" ]; then
  pkill -f component_container 2>/dev/null; sleep 2
  nohup ros2 launch dmp_ros2_bridge dmp_bridge_composed.launch.py devices:=bed01,bed02 \
       mt:=true gateway_port:="$GW_PORT" gw_overrides:=bed01=127.0.0.1:0 \
       > "$LOG/isolate.log" 2>&1 & ILP=$!
  sleep 10
  timeout 40 ros2 lifecycle set /dmp_bridge_bed01 configure >/dev/null 2>&1
  timeout 40 ros2 lifecycle set /dmp_bridge_bed02 configure >/dev/null 2>&1
  timeout 40 ros2 lifecycle set /dmp_bridge_bed02 activate  >/dev/null 2>&1
  sleep 2
  chk "bed01 被拒后停在 unconfigured(不是一路推到 active)" \
      "$(timeout 15 ros2 lifecycle get /dmp_bridge_bed01 2>/dev/null | awk '{print $1}')" "unconfigured"
  chk "bed01 失败没毁容器: 容器进程还在" "$(kill -0 "$ILP" 2>/dev/null && echo 1 || echo 0)" "1"
  chk "bed01 失败后它的三个服务确实不存在(configure 没走完就没建)" \
      "$(timeout 15 ros2 service list 2>/dev/null | grep -c '^/dmp/bed01/set_rule$')" "0"
  chk "bed01 失败不传染: bed02 的 change_state 仍在且能激活" \
      "$(timeout 15 ros2 lifecycle get /dmp_bridge_bed02 2>/dev/null | awk '{print $1}')" "active"
  recorder 8 "$LOG/gap_isolate.json" >/dev/null 2>&1
  ISO_N=$(python3 -c "import json;print(sum(json.load(open('$LOG/gap_isolate.json'))['count'].values()))" 2>/dev/null)
  echo "  故障期同容器另一台的数据面采样: $(cat "$LOG/gap_isolate.json" 2>/dev/null)"
  [ "${ISO_N:-0}" -gt 20 ] && { echo "  PASS 一台配置失败, 另一台照发($ISO_N 批)"; PASS=$((PASS+1)); } \
                           || { echo "  FAIL 另一台只有 ${ISO_N:-?} 批 (隔离没成立, 或 bed02 根本没激活)"; FAIL=$((FAIL+1)); }
  echo "  日志里那句拒因(给运维看的, 不是给断言看的): $(grep -o 'configure 拒绝[^)]*' "$LOG/isolate.log" | head -1)"
  kill "$ILP" 2>/dev/null; pkill -f component_container 2>/dev/null; sleep 2
fi

echo "=== [8] 汇总 ==="
echo "  PASS=$PASS FAIL=$FAIL   (Q3 的状态机断言全部走这几个 PASS/FAIL, 不靠人眼看日志)"
echo "  Q4 的结论不在这里: 它是一组毫秒数, 看上面 mt / st 两行自己比。"
echo "  Q6/Q7 同上: [6] 的表格与 [7] 的 PASS/FAIL 才是结论, 本脚本不替它们说话。"
pkill -f component_container 2>/dev/null; pkill -f bridge_node 2>/dev/null
kill "$GW" "$SIM" 2>/dev/null
[ "$FAIL" -eq 0 ] && echo "ALL_LIFECYCLE_ASSERTS_PASS"
echo "SCRIPT_DONE"
