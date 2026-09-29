#!/usr/bin/env bash
# vm_downstream_bag.sh — 下游联动(e1): 让**真实 ROS2 生态组件**消费本项目的话题, 而不是自己写消费者自证
#
# 为什么选 rosbag2: 它是 ROS2 核心发行版里的组件, 不看本项目一行代码。它能录下 /dmp/frames 并
# 原样回放, 就说明"自定义 rosidl 类型 + 我们的桥"在生态工具链里是真的可互操作 —— 这比"我写了个
# 订阅节点收到数据"有力得多(那个订阅节点是我自己写的)。
#
# 三格互相咬合, 单独任何一格都不算证据:
#   [1] 实时侧(我自己写的 best_effort 订阅方)在录的同时逐条落 csv   -> 拿到"应有的样子"
#   [2] rosbag2 录制 -> 用 rosbag2_py 把 bag 里的字节反序列化回 csv   -> 与 [1] 逐字节 cmp
#   [3] ros2 bag play 回放 -> 另一个实时订阅方再落 csv               -> 与 [2] 逐字节 cmp
#   少 [1] 则 [2] 的"零条"永远为真(素材录 B-27); 少 [3] 则"存进去了"不等于"取出来还是它"。
#
# [4] 是一格**预设了再跑**的对照: 不给 QoS override 直接 ros2 bag record。
#   跑之前的预设(摁住不动): rosbag2 默认以 reliable 订阅, 而桥默认发 best_effort(sensor) ——
#   按本项目实测过的错配方向(素材录 B-07 的四格矩阵), 应当是**静默零条**。
#   2026-09-29 首跑实测 141 批: **预设被推翻**。Humble 的 rosbag2 会查发布端 offered QoS 并
#   自建兼容订阅 —— 生态工具比我手写的订阅方聪明。新预设与证据都在[4]那一格边上写着。
#
# [6] diagnostic_aggregator: 让**标准诊断栈**（不是我自己写的消费者）去订阅 /diagnostics 并把
#   结果重新发布出来。2026-09-29 之前这格一直 SKIP(本机 sudo 要密码, apt 装不上); 包装上后补成真断言。
#   侦查过程在 tools/diagnostics/vm_aggregator_probe.sh —— 那一轮自己的读数被残留进程污染过,
#   所以本格开头有一句"节点必须只有一个"的断言, 不是凑数, 是被这件事教过(见下面 [6] 的注释)。
#
# 用法(VM 内): bash tools/vm_downstream_bag.sh
set +u
set +e
source /opt/ros/humble/setup.bash
# 不要在 source 完ROS 后就把 set -u 开回来: ament 生成的 install/setup.bash 里读了 COLCON_TRACE
# 这类未必存在的变量。素材录 B-14 记的是 /opt/ros 那一份, 本轮(2026-09-29 首跑)在
# install/setup.bash:11 又撞了一次 —— 同一条不兼容对**每个** ament 脚本都成立, 不是某一个的毛病。
# 代价只是少一个拼写检查; 换来的是一句无声死在环境加载里的 rc=1 与 60 字节日志。
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG="${LOG:-/tmp/dmp_downstream}"
GW_PORT="${GW_PORT:-9100}"
SIM_PORT="${SIM_PORT:-9000}"
rm -rf "$LOG"; mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f bridge_node 2>/dev/null
pkill -f 'ros2 bag' 2>/dev/null
# 按名字收, 不用 $! : 上一轮侦查实测 `kill $PID` 只杀掉了 `ros2 run` 的外壳,
# aggregator_node / example_pub.py 子进程照旧活着, 于是下一个阶段的读数里混着上一个阶段的
# 发布物(具体到这里: /diagnostics_agg 被 example 的 /Aggregation/* 占满)。素材录 B-40 跨轮污染
# 的**同族**, 只是这次发生在同一个脚本的两个阶段之间, 不是两轮之间。
pkill -f aggregator_node 2>/dev/null
pkill -f example_pub.py 2>/dev/null
sleep 1
PASS=0; FAIL=0; SKIP=0; PRED_FAIL=0
chk() {  # chk <说明> <实际> <期望>
  if [ "$2" = "$3" ]; then echo "  PASS $1 (=$2)"; PASS=$((PASS+1));
  else echo "  FAIL $1: 实际=$2 期望=$3"; FAIL=$((FAIL+1)); fi
}
# [4] 是"先写下预设再跑"的格, 所以**不能**和缺陷共用一个计数器: 预设被推翻是新事实(该改文档),
# 不是链路坏了。混成一个 FAIL 之后, 人总有冲动去把期望值改成实测值 —— 那就成了自证。
chk_pred() {  # chk_pred <说明> <实测> <预设>
  if [ "$2" = "$3" ]; then echo "  PASS(预设命中) $1 (=$2)"; PASS=$((PASS+1));
  else echo "  PRED-OVERTURNED $1: 预设=$3 实测=$2 —— 新事实, 要改的是文档和认知, 不是断言"
       PRED_FAIL=$((PRED_FAIL+1)); fi
}

# 比**窗口**而不是比整个文件: 三个采集窗天然长短不一(bag 录 16s, 实时订阅方只盯 8s)。
# 若直接 cmp 两个 csv, 边界不同会**必然**不等 —— 那是脚本自己的错, 拿来当链路结论就是假红
# (素材录 B-37 的同族: 统计量的定义没对齐, 数字就只是在比谁截得多)。
# 判据: 短的一侧(A)每一行, 按 seq 在长的一侧(B)里都能找到且**六个字段逐一相同**。
win_cmp() {  # win_cmp <A.csv 应被包含> <B.csv 全集> <名字>; stdout=1/0, stderr=明细
  python3 - "$1" "$2" "$3" <<'PY'
import csv, sys
name = sys.argv[3]
def load(p):
    try:
        return [tuple(r) for r in csv.reader(open(p)) if r]
    except OSError:
        return []
a, b = load(sys.argv[1]), load(sys.argv[2])
bi = {r[1]: r for r in b}          # seq -> 整行
bad = [r for r in a if r[1] not in bi or bi[r[1]] != r]
print('%s: A=%d B=%d 找不到=%d 字段不同=%d 首个差异=%s' % (
    name, len(a), len(b),
    sum(1 for r in a if r[1] not in bi),
    sum(1 for r in a if r[1] in bi and bi[r[1]] != r),
    ','.join(bad[0]) if bad else '-'), file=sys.stderr)
print('1' if len(a) >= 10 and not bad else '0')
PY
}

# 逐条落 csv: frame_id,seq,channel,kind,kind_name,value。value 用 repr() —— 两侧必须用**同一个**
# 格式化函数, 否则"逐字节相等"这件事就成了比谁的四舍五入更像。
dump_frames() {  # dump_frames <输出csv> <秒> <节点名>
  timeout -s INT $(( $2 + 4 )) python3 - "$1" "$2" "${3:-dmp_ds_probe}" > "$1.meta" 2>"$1.err" <<'PY'
import csv, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from dmp_msgs.msg import DeviceFrameArray
out, dur = sys.argv[1], float(sys.argv[2])
rows = []
seen = [0]                       # 批数必须真数出来: 之前这里写 getattr(n,'_b',0), 而 _b 从未被赋值,
                                 # 于是日志里永远印一句 batches_seen=0 —— 一个凭空造的读数(B-43 同族)。
def cb(m):
    seen[0] += 1
    for f in m.frames:
        rows.append((f.header.frame_id, f.seq, f.channel, f.kind, f.kind_name, repr(f.value)))
rclpy.init(); n = Node(sys.argv[3])
n.create_subscription(DeviceFrameArray, '/dmp/frames', cb,
                      QoSProfile(depth=50, reliability=ReliabilityPolicy.BEST_EFFORT))
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    rclpy.spin_once(n, timeout_sec=0.05)
with open(out, 'w', newline='') as fp:
    csv.writer(fp).writerows(rows)
print('batches_seen=%d frames=%d first_seq=%s last_seq=%s' % (
    seen[0], len(rows),
    rows[0][1] if rows else '-', rows[-1][1] if rows else '-'))
PY
}

# 两侧**同窗**抓: 原始 /diagnostics 与聚合 /diagnostics_agg。分两个窗口抓就变成"比两个时刻的诊断栈",
# 那条"聚合前后 level 一致"的断言当场就没有意义了。
agg_dump() {  # agg_dump <输出csv> <秒>
  timeout -s INT $(($2 + 6)) python3 - "$1" "$2" > "$1.meta" 2>"$1.err" <<'PY'
import csv, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from diagnostic_msgs.msg import DiagnosticArray
out, dur = sys.argv[1], float(sys.argv[2])
rows = []
def lv_of(s):
    # rclpy 把消息里的 uint8 字段反序列化成 **bytes**(不是 int): DiagnosticStatus.level 拿到的是 b'\x00'。
    # 2026-09-29 本轮实测: 直接写 int(s.level) 抛 ValueError, 异常从回调里冒出去把 spin 整个打断,
    # 于是两侧都收到 0 项 —— 一个脚本自己的 bug 看上去完全像"聚合栈没工作"。
    # 正因为同一格还并列了一个**原始侧**的阳性对照, 才能当场看出错在我这里而不是链路那里。
    v = s.level
    return v[0] if isinstance(v, (bytes, bytearray)) and len(v) == 1 else int(v)
def mk(tag):
    def cb(m):
        for s in m.status:
            rows.append((tag, s.name, lv_of(s), s.hardware_id, s.message))
    return cb
rclpy.init(); n = Node('dmp_agg_probe')
qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
n.create_subscription(DiagnosticArray, '/diagnostics',     mk('raw'), qos)
n.create_subscription(DiagnosticArray, '/diagnostics_agg', mk('agg'), qos)
t0 = time.monotonic()
while time.monotonic() - t0 < dur:
    rclpy.spin_once(n, timeout_sec=0.05)
with open(out, 'w', newline='') as fp:
    csv.writer(fp).writerows(rows)
print('raw_status=%d agg_status=%d raw_uniq=%d agg_uniq=%d' % (
    sum(1 for r in rows if r[0] == 'raw'), sum(1 for r in rows if r[0] == 'agg'),
    len(set(r[1] for r in rows if r[0] == 'raw')), len(set(r[1] for r in rows if r[0] == 'agg'))))
PY
}

# 把一份 csv 只读**一次**, 一次吐出全部 KEY=VAL。先前想过按断言条数各跑一遍 python:
# 那不只慢, 还让每条断言各自解析一次 -> 一处解析写错, 几格红的不是链路而是脚本自己。
agg_judge() {  # agg_judge <csv> -> 一行 KEY=VAL ...
  python3 - "$1" <<'PY'
import csv, re, sys
rows = [r for r in csv.reader(open(sys.argv[1])) if r and len(r) == 5]
raw, agg = {}, {}
for tag, name, level, hw, _msg in rows:
    (raw if tag == 'raw' else agg)[name] = (level, hw)   # 同窗内反复发布, 留最后一次读数
# 聚合后的条目名形如 /DMP/Channels/CONC_ch3(analyzer 的 path 在前, 条目名原样拼在后)。
# 这个形状是 2026-09-29 实跑量出来的; 当时先照厂商 example 写成"path + '/ ' + name"(它们那行是
# /Aggregation/Arms/ arms left motor, 斜杠后带个空格) —— 我们这里**不带空格**, 因为带不带空格
# 取决于上游 status 自己的 name 长什么样, 不是一个可以依赖的约定。所以这里用"以原始 name 结尾"去认,
# 而不是把两种形状都写进期望值。
link = [k for k in agg if k.endswith('telemetry_link')]
chan = [k for k in agg if re.search(r'_ch\d+$', k)]
cmped = [(r, a) for r, (lv, _h) in raw.items() for a in agg if a.endswith(r)]
mism  = [(r, a) for r, (lv, _h) in raw.items() for a in agg
         if a.endswith(r) and agg[a][0] != lv]
hwkeep = sum(1 for _r, _v in raw.items()
             for a in agg if a.endswith(_r) and agg[a][1] != '')
print('RAW_N=%d AGG_N=%d LINK_IN=%d CH_IN=%d CMP=%d MISMATCH=%d HW_KEEP=%d' % (
    len(raw), len(agg), len(link), len(chan), len(cmped), len(mism), hwkeep))
print('AGG_SAMPLE=%s' % ('|'.join(sorted(agg)[:4]) or '-'), file=sys.stderr)
PY
}
# 收不到东西时回 0 而不是回空: 空串会让下面的 `python3 -c "print(1 if  > 0 ...)"` 直接语法错,
# 于是一格红看起来像"脚本坏了"而不是"链路没数据" —— 读数缺失要能诊断, 不能变成另一种噪声。
g() { local v; v=$(echo "$1" | tr ' ' '\n' | grep -m1 "^$2=" | cut -d= -f2); echo "${v:-0}"; }

chk_pred_agg() {  # chk_pred_agg <说明> <实测> <预设> —— 与 [4] 同一纪律: 预设没中是新事实, 不是缺陷
  if [ "$2" = "$3" ]; then echo "  PASS(预设命中) $1 (=$2)"; PASS=$((PASS+1));
  else echo "  PRED-OVERTURNED $1: 预设=$3 实测=$2 —— 新事实, 要改的是文档和认知, 不是断言"
       PRED_FAIL=$((PRED_FAIL+1)); fi
}

echo "=== [0] simulator + gateway + 桥(autostart 走完 configure->activate) ==="
nohup ./build_linux/device_simulator "$SIM_PORT" > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service "$GW_PORT" 127.0.0.1 "$SIM_PORT" > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2
DEV_ID=bed01
NODE=dmp_bridge_$DEV_ID
nohup ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=$DEV_ID gateway_port:="$GW_PORT" \
     > "$LOG/bridge.log" 2>&1 & LAUNCH=$!
sleep 12
chk "桥真的起来了(下游一切的前提, 不是碰巧有数据)" \
    "$(timeout 20 ros2 lifecycle get /$NODE 2>/dev/null | awk '{print $1}')" "active"

echo "=== [1][2] 同一窗口内: 我自己订阅落 csv + rosbag2 录制 ==="
cat > "$LOG/qos.yaml" <<'YAML'
/dmp/frames:
  reliability: best_effort
  history: keep_last
  depth: 50
YAML
nohup timeout -s INT 16 ros2 bag record -o "$LOG/bag" -s sqlite3 \
      --qos-profile-overrides-path "$LOG/qos.yaml" /dmp/frames > "$LOG/rec.log" 2>&1 & BAGJ=$!
sleep 3
dump_frames "$LOG/live.csv" 8 dmp_live_probe
wait $BAGJ 2>/dev/null
sleep 1
LIVE_N=$(cut -d, -f2 "$LOG/live.csv" 2>/dev/null | wc -l)
chk "阳性对照: 实时侧这段时间确实在发 ($LIVE_N 帧) —— 否则下面 bag 的条数说明不了任何事" \
    "$(python3 -c "print(1 if ${LIVE_N:-0}>10 else 0)")" "1"
chk "rosbag2 录出了存储目录" "$( [ -f "$LOG/bag/metadata.yaml" ] && echo 1 || echo 0 )" "1"
BAG_N=$(awk '/message_count:/{s+=$2} END{print s+0}' "$LOG/bag/metadata.yaml" 2>/dev/null)
chk "bag 里批数>0 (=${BAG_N:-0} 批)" "$(python3 -c "print(1 if ${BAG_N:-0}>10 else 0)")" "1"
chk "bag 认得这是本项目的自定义类型 (不是 generic msg 兜底)" \
    "$(grep -c 'dmp_msgs/msg/DeviceFrameArray' "$LOG/bag/metadata.yaml" 2>/dev/null)" "1"

echo "=== [4] 对照格: 不给 QoS override 直接录(预设见文件头) ==="
timeout -s INT 10 ros2 bag record -o "$LOG/bag_noqos" -s sqlite3 /dmp/frames > "$LOG/rec_noqos.log" 2>&1
NOQOS_N=$(awk '/message_count:/{s+=$2} END{print s+0}' "$LOG/bag_noqos/metadata.yaml" 2>/dev/null)
echo "  本轮读数: 不给 override 录到 ${NOQOS_N:-?} 批 (rosbag2 日志尾部: $(tail -2 "$LOG/rec_noqos.log" 2>/dev/null | tr '\n' '|'))"
# 预设的来历(两段都留着, 不许只留新的):
#   跑之前我按素材录 B-07 的四格矩阵写下"reliable 订阅对 best_effort 发布 -> 静默零条";
#   2026-09-29 首跑实测 141 批 —— 预设被推翻: Humble 的 rosbag2 会查发布端 offered QoS 并自建兼容订阅。
#   改期望值只在**能解释**之后允许(否则就是把期望改成实测那一套自证)。新预设仍然可证伪:
#   哪天这格变回 0, 要么 rosbag2 的探测行为变了, 要么桥的 QoS 变了 —— 两者都得重跑 B-07 那张表。
NOQOS_EXPECT="${NOQOS_EXPECT:-1}"   # 1=应当收到(>10 批); 0=应当静默零条(旧预设, 留着做对照)
chk_pred "对照格: 不给 override 也收到了(rosbag2 自动探测 offered QoS)" \
    "$(python3 -c "print(1 if ${NOQOS_N:-0}>10 else 0)")" "$NOQOS_EXPECT"

echo "=== [3] 把 bag 里的字节反序列化回 csv, 与实时侧逐字节比 ==="
timeout -s INT 30 python3 - "$LOG/bag" "$LOG/bag.csv" > "$LOG/bagread.err" 2>&1 <<'PY'
import csv, sys
import rosbag2_py
from rclpy.serialization import deserialize_message
from dmp_msgs.msg import DeviceFrameArray
reader = rosbag2_py.SequentialReader()
reader.open(rosbag2_py.StorageOptions(uri=sys.argv[1], storage_id='sqlite3'),
            rosbag2_py.ConverterOptions('cdr', 'cdr'))
rows = []
while reader.has_next():
    topic, data, _ts = reader.read_next()
    m = deserialize_message(data, DeviceFrameArray)
    for f in m.frames:
        rows.append((f.header.frame_id, f.seq, f.channel, f.kind, f.kind_name, repr(f.value)))
with open(sys.argv[2], 'w', newline='') as fp:
    csv.writer(fp).writerows(rows)
print('bag_frames=%d' % len(rows))
PY
BAGF=$(cut -d, -f2 "$LOG/bag.csv" 2>/dev/null | wc -l)
echo "  实时侧 $LIVE_N 帧 / bag 内 $BAGF 帧"
chk "bag 完整包含了实时侧每一帧且六字段逐字相同" "$(win_cmp "$LOG/live.csv" "$LOG/bag.csv" live-vs-bag)" "1"

echo "=== [6] diagnostic_aggregator: 让标准诊断栈聚合我们的 /diagnostics ==="
# 为什么放在 [3] 之后、[4b] 之前: /diagnostics 只在桥 active 时流, 而 [4b] 为了回放静音会把桥
# deactivate。顺序写错的话这格收到的永远是超时项 —— 那不是链路红, 是脚本自己造成的假红。
# 参数文件的形状照搬厂商 example(顶层键就是 `analyzers`, 实测 ros2 node list 得到的节点名正是 /analyzers):
#   /opt/ros/humble/share/diagnostic_aggregator/example_analyzers.yaml
# 聚合输出话题 /diagnostics_agg, 顶层汇总 /diagnostics_toplevel_state —— 两个都是侦查轮量出来的。
# 读数里 AGG_N 比 RAW_N 多出的那几个不是新告警, 是聚合树自己的分组节点(/DMP 与每个 analyzer 一个):
# 2026-09-29 本轮原始 5 项 / 聚合 8 项, 多的 3 个就正对 /DMP、/DMP/Link、/DMP/Channels。
cat > "$LOG/agg.yaml" <<'YAML'
analyzers:
  ros__parameters:
    path: DMP
    link:
      type: diagnostic_aggregator/GenericAnalyzer
      path: Link
      startswith: ['telemetry']
      timeout: 5.0
    channels:
      type: diagnostic_aggregator/GenericAnalyzer
      path: Channels
      contains: ['_ch']
      timeout: 5.0
YAML
nohup ros2 run diagnostic_aggregator aggregator_node --ros-args \
      --params-file "$LOG/agg.yaml" > "$LOG/agg.log" 2>&1 & AGG=$!
sleep 6
# 先问"谁在答"再问"答得对不对": 侦查轮就是在这里吃过亏 —— 上一个阶段的聚合器没死干净,
# 两个节点同名 /analyzers 、都发 /diagnostics_agg, 而 echo 拿到的全是对方那份。
chk "图上只有**一个**聚合器节点(多个同名单元 = 读数出处说不清)" \
    "$(timeout 20 ros2 node list 2>/dev/null | grep -c '^/analyzers$')" "1"
chk "诊断栈的顶层汇总话题在(外部监控就看这一个)" \
    "$(timeout 20 ros2 topic list 2>/dev/null | grep -c '^/diagnostics_toplevel_state$')" "1"
agg_dump "$LOG/agg_both.csv" 10
KV=$(agg_judge "$LOG/agg_both.csv" 2>"$LOG/agg_sample.txt")
echo "  读数: $KV"
echo "  探针侧: $(cat "$LOG/agg_both.csv.meta" 2>/dev/null | tr '\n' '|') err尾行: $(tail -1 "$LOG/agg_both.csv.err" 2>/dev/null)"
echo "  聚合项样例: $(grep -m1 '^AGG_SAMPLE=' "$LOG/agg_sample.txt" 2>/dev/null | cut -d= -f2-)"
chk "阳性对照: 同窗两侧都收到了东西 (原始 $(g "$KV" RAW_N) 项 / 聚合 $(g "$KV" AGG_N) 项)" \
    "$(python3 -c "print(1 if $(g "$KV" RAW_N) > 0 and $(g "$KV" AGG_N) > 0 else 0)")" "1"
chk "我们的 telemetry_link 进了聚合结果 ($(g "$KV" LINK_IN) 项)" \
    "$(python3 -c "print(1 if $(g "$KV" LINK_IN) > 0 else 0)")" "1"
chk "通道项 *_chN 进了聚合结果 ($(g "$KV" CH_IN) 项)" \
    "$(python3 -c "print(1 if $(g "$KV" CH_IN) > 0 else 0)")" "1"
# 这条不写进文件头当预设: 它就是普通断言。聚合器如果改了我们的 level, 那是它把链路判错了,
# 不是"我预先没料到" —— 医疗语境下这正是最该红的一格。
chk "聚合前后同名项 level 逐条一致 (比了 $(g "$KV" CMP) 条, 不一致 $(g "$KV" MISMATCH) 条)" \
    "$(python3 -c "print(1 if $(g "$KV" CMP) >= 1 and $(g "$KV" MISMATCH) == 0 else 0)")" "1"
HW_EXPECT="${HW_EXPECT:-1}"   # 1=聚合项应当保留我们的 hardware_id=bed01
chk_pred_agg "预设: 聚合不丢 hardware_id(多台设备共用一个聚合器时靠它分辨)" \
    "$(python3 -c "print(1 if $(g "$KV" HW_KEEP) > 0 else 0)")" "$HW_EXPECT"

echo "=== [6b] 把一条阈值挪到现值之外: 非 OK 能不能沿标准诊断栈传出去 ==="
# 为什么还要这一格: [6] 那七条只证到"我们的项被聚合、level 逐条透传", 而那一窗里**全部**是 OK(0)。
# 0 对 0 的一致是**弱证据** —— “告警沿标准诊断栈传播”这句话, 没有非 OK 那一侧就不能算验证过。
# 手段用桥自己的服务(不改固件、不改网关代码): 先读当前现值与阈值, 再把区间挪到现值之外。
# 注意这是**双向**注入: 所以必须先把原始侧推到非 OK 当作本格的阳性对照, 否则下面那条"聚合侧也非 OK"
# 又掉回 B-27 那个形状(一个永远为真的断言)。末尾把阈值恢复回去: 不把改造过的现场留给后面几格。
IKV=$(timeout -s INT 90 python3 - "$DEV_ID" 2>"$LOG/inject.err" <<'PY' | tail -1
import re, sys, time
import rclpy
from rclpy.node import Node
from rclpy.qos import QoSProfile, ReliabilityPolicy
from diagnostic_msgs.msg import DiagnosticArray
from dmp_msgs.srv import GetRules, SetRule
dev = sys.argv[1]
rclpy.init(); n = Node('dmp_agg_inject')
def lv_of(s):                      # 同一个 uint8->bytes 的坑, 见 [6] 的注释
    v = s.level
    return v[0] if isinstance(v, (bytes, bytearray)) and len(v) == 1 else int(v)
qos = QoSProfile(depth=10, reliability=ReliabilityPolicy.RELIABLE)
raw, agg = {}, {}
def on_raw(m):
    for s in m.status:
        raw[s.name] = (lv_of(s), s.hardware_id, {kv.key: kv.value for kv in s.values})
def on_agg(m):
    for s in m.status:
        agg[s.name] = lv_of(s)
n.create_subscription(DiagnosticArray, '/diagnostics',     on_raw, qos)
n.create_subscription(DiagnosticArray, '/diagnostics_agg', on_agg, qos)
def spin(sec):
    t0 = time.monotonic()
    while time.monotonic() - t0 < sec:
        rclpy.spin_once(n, timeout_sec=0.05)
spin(4)
cand = [k for k in raw if re.search(r'_ch\d+$', k) and raw[k][0] == 0
        and raw[k][2].get('rule', '-') != '-' and 'value' in raw[k][2]]
if not cand:
    print('INJ_RAW_LVL=-1 INJ_AGG_LVL=-1 INJ_ACCEPT=0 RESTORE=0'); sys.exit()
name = sorted(cand)[0]
kind_name = name.rsplit('_ch', 1)[0]
value = float(raw[name][2]['value'])
cl = n.create_client(GetRules, '/dmp/%s/get_rules' % dev)
if not cl.wait_for_service(timeout_sec=10):
    print('INJ_RAW_LVL=-1 INJ_AGG_LVL=-1 INJ_ACCEPT=0 RESTORE=0'); sys.exit()
f = cl.call_async(GetRules.Request())
rclpy.spin_until_future_complete(n, f, timeout_sec=15)
rules = f.result().rules if f.result() is not None else []
kind, orig = None, None
for r in rules:
    if r.kind_name.lower() == kind_name.lower():
        kind, orig = int(r.kind), (float(r.low), float(r.high))
if kind is None:
    print('INJ_RAW_LVL=-1 INJ_AGG_LVL=-1 INJ_ACCEPT=0 RESTORE=0'); sys.exit()
sc = n.create_client(SetRule, '/dmp/%s/set_rule' % dev)
sc.wait_for_service(timeout_sec=10)
def call(low, high, msg):
    q = SetRule.Request(); q.kind = kind; q.low = low; q.high = high; q.message = msg
    ff = sc.call_async(q)
    rclpy.spin_until_future_complete(n, ff, timeout_sec=20)
    return 1 if (ff.result() is not None and ff.result().accepted) else 0
# 区间整段抬到现值之上: 按 levelFor 的定义, 越界就是 Error(2), 不靠预警带那种边界情形
accept = call(value + 1000.0, value + 2000.0, 'e3-forced-out-of-range')
spin(6)
agg_key = next((k for k in agg if k.endswith(name)), '')
raw_lvl = raw[name][0] if name in raw else -1
agg_lvl = agg.get(agg_key, -1)
restore = call(orig[0], orig[1], 'e3-restore')
print('INJ_RAW_LVL=%d INJ_AGG_LVL=%d INJ_ACCEPT=%d RESTORE=%d' % (raw_lvl, agg_lvl, accept, restore))
PY
)
echo "  读数: $IKV  (详见 $LOG/inject.err)"
chk "注入确实把**原始侧**那条推到非 OK(阳性对照: 不然下一条又永远为真)" \
    "$(python3 -c "print(1 if $(g "$IKV" INJ_RAW_LVL) > 0 else 0)")" "1"
chk "同一个非 OK 出现在聚合侧, 且与原始侧同值（告警沿标准诊断栈传出去）" \
    "$(python3 -c "a=$(g "$IKV" INJ_RAW_LVL); b=$(g "$IKV" INJ_AGG_LVL); print(1 if a>0 and a==b else 0)")" "1"
chk "阈值已恢复原值(不把改造过的现场留给后面几格)" "$(g "$IKV" RESTORE)" "1"

echo "=== [4b] 回放前先把实时流关静默(不然 play.csv 的出处说不清) ==="
# 首跑这里 FAIL 过: `kill -INT $LAUNCH` + sleep 4 后探针仍收到 64 帧。
# 不猜原因, 分成两件事量: (i) 静音用生命周期门禁 —— on_deactivate 停定时器就是为此设计的,
# 不依赖信号的传递语义; (ii) 把"launch 到底众多久才退"只**量不答**(compose [8] 里已经有
# SIGINT 自退的正式断言, 这里重复断言只会让两格谁红了分不清)。
timeout 30 ros2 lifecycle set /$NODE deactivate 2>&1 | tail -1
chk "桥已处于 inactive(静音是门禁造成的, 不是网络碰巧)" \
    "$(timeout 15 ros2 lifecycle get /$NODE 2>/dev/null | awk '{print $1}')" "inactive"
SILENT=$(dump_frames "$LOG/silent.csv" 3 dmp_silent_probe >/dev/null 2>&1; cut -d, -f2 "$LOG/silent.csv" 2>/dev/null | wc -l)
chk "deactivate 后这 3s 实时侧确实 0 帧 (否则下面回放读到什么都不知是活的)" "${SILENT:-0}" "0"
# 本格的预算就是这两条断言(状态 + 0 帧), 不再多 kill 一次: 上一轮量到的事实是
# `kill -INT $LAUNCH` 15s 内没让 launch 退 —— 那是 launch 的信号传递问题, 不该拖这里的计时。
# 收尾在 [9b]: 按名字收, 并量一个"收完还在几个"。

echo "=== [5] 回放: ros2 bag play, 另一个订阅方再落一次 csv ==="
nohup timeout -s INT 14 ros2 bag play "$LOG/bag" > "$LOG/play.log" 2>&1 & PLAY=$!
sleep 2
dump_frames "$LOG/play.csv" 10 dmp_play_probe
wait $PLAY 2>/dev/null
PLAYF=$(cut -d, -f2 "$LOG/play.csv" 2>/dev/null | wc -l)
# 先立阳性对照再比内容: play.csv 空或很短时"每一帧都能找到"会永远为真(素材录 B-27)。
chk "阳性对照: 回放侧真收到了东西 ($PLAYF 帧)" \
    "$(python3 -c "print(1 if ${PLAYF:-0}>10 else 0)")" "1"
chk "回放出来的每一帧都能回到 bag 里逐字段相同" "$(win_cmp "$LOG/play.csv" "$LOG/bag.csv" play-vs-bag)" "1"
echo "  回放侧 $PLAYF 帧; play 日志尾部: $(grep -o 'duration:[^ ]*' "$LOG/play.log" 2>/dev/null | head -1)"

echo "=== [9b] 收尾读数: 别把进程留给下一轮 ==="
# 为什么不是只靠 kill -INT: 上一轮实测 launch 收到 SIGINT 后 15s 都没退。脚本开头那句
# pkill -f bridge_node 只能杀掉节点本体, launch 自己的 python 进程会一轮一轮堆下去 ——
# 残留进程就是 B-40 那一类跨轮污染。这里按**名字**收, 并量一个"收完还在几个"的读数;
# 模式串不会匹到自己(本脚本叫 vm_downstream_bag.sh, 素材录 B-08)。
kill $GW $SIM 2>/dev/null
kill $AGG 2>/dev/null          # 只 kill 外壳不够, 下面还按名字收一道
pkill -f aggregator_node 2>/dev/null
pkill -f dmp_bridge.launch.py 2>/dev/null; pkill -f bridge_node 2>/dev/null
sleep 2
LEFT=$(pgrep -f 'bridge_node|dmp_bridge.launch.py' | wc -l)
LEFT2=$(pgrep -f 'build_linux/(device_simulator|gateway_service)' | wc -l)
LEFT3=$(pgrep -f 'aggregator_node' | wc -l)
echo "  读数: 桥相关进程仍在 $LEFT 个; sim/gateway 仍在 $LEFT2 个; 聚合器仍在 $LEFT3 个 (三者都应当 0)"
chk "收尾收干净了(不给下一轮埋跨轮污染)" "$(python3 -c "print(1 if ${LEFT:-0}==0 and ${LEFT2:-0}==0 and ${LEFT3:-0}==0 else 0)")" "1"

echo "=== [9] 汇总 ==="
echo "  PASS=$PASS FAIL=$FAIL SKIP=$SKIP PRED_OVERTURNED=$PRED_FAIL"
[ "$SKIP" -gt 0 ] && echo "  注: SKIP 不算通过。本脚本对『告警沿标准诊断栈传播』这件事**没有任何证据**, 只有 bag 那几格。"
[ "$PRED_FAIL" -gt 0 ] && echo "  注: PRED_OVERTURNED 不是缺陷, 是预设被现实推翻 —— 必须写进文档, 不许把期望改成实测。"
if [ "$FAIL" = 0 ] && [ "$PASS" -gt 0 ]; then
  if [ "$PRED_FAIL" -gt 0 ]; then echo "ALL_DOWNSTREAM_BAG_ASSERTS_PASS  (注: 另有 $PRED_FAIL 格预设被推翻, 见上面 PRED-OVERTURNED 行)"; else echo "ALL_DOWNSTREAM_BAG_ASSERTS_PASS"; fi
fi
echo SCRIPT_DONE

# 收尾: 进程已在 [9b] 按名字收并量过读数; 这里只对 sim/gateway/聚合器再补一刀(幂等)。
kill $GW $SIM $AGG 2>/dev/null
pkill -f aggregator_node 2>/dev/null
exit 0
