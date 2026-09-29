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
#   预设(写完先按住): rosbag2 默认以 reliable 订阅, 而桥默认发 best_effort(sensor) ——
#   按本项目实测过的错配方向(素材录 B-07 的四格矩阵), 应当是**静默零条**。
#   如果这格报 FAIL(收到了数据), 说明 Humble 的 rosbag2 会自己探测发布端 QoS, 那是**新事实**,
#   要改文档而不是改断言。
#
# [6] diagnostic_aggregator: 本机 sudo 需要密码, apt 装不上 -> 显式 SKIP 并打出安装命令。
#   SKIP 不是 PASS: 汇总行会把"未验证"单独说出来(素材录 B-35 —— 结论行必须从本轮数据现算)。
#
# 用法(VM 内): bash tools/vm_downstream_bag.sh
set +u
set +e
source /opt/ros/humble/setup.bash
set -u
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

echo "=== [0] simulator + gateway + 桥(autostart 走完 configure->activate) ==="
nohup ./build_linux/device_simulator "$SIM_PORT" > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service "$GW_PORT" 127.0.0.1 "$SIM_PORT" > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2
NODE=dmp_bridge_bed01
nohup ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_port:="$GW_PORT" \
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

echo "=== [4] 对照格: 不给 QoS override 直接录(预设=静默零条, 见文件头) ==="
timeout -s INT 10 ros2 bag record -o "$LOG/bag_noqos" -s sqlite3 /dmp/frames > "$LOG/rec_noqos.log" 2>&1
NOQOS_N=$(awk '/message_count:/{s+=$2} END{print s+0}' "$LOG/bag_noqos/metadata.yaml" 2>/dev/null)
echo "  本轮读数: 不给 override 录到 ${NOQOS_N:-?} 批 (rosbag2 日志尾部: $(tail -2 "$LOG/rec_noqos.log" 2>/dev/null | tr '\n' '|'))"
echo "  预设(跑之前按住不动): Humble 的 rosbag2 若以 reliable 订阅, 对 best_effort 发布应当静默零条"
chk_pred "对照格与[2]反向: 不给 override 应当收不到(预设=0 批)" "${NOQOS_N:-0}" "0"

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

echo "=== [4b] 回放前先证实时侧已静默(不然 play.csv 的出处说不清) ==="
kill -INT $LAUNCH 2>/dev/null; sleep 4
SILENT=$(dump_frames "$LOG/silent.csv" 3 dmp_silent_probe >/dev/null 2>&1; cut -d, -f2 "$LOG/silent.csv" 2>/dev/null | wc -l)
chk "桥已停: 这 3s 收到 0 帧 (否则下面回放读到什么都不知是活的)" "${SILENT:-0}" "0"

echo "=== [5] 回放: ros2 bag play, 另一个订阅方再落一次 csv ==="
nohup timeout -s INT 14 ros2 bag play "$LOG/bag" > "$LOG/play.log" 2>&1 & PLAY=$!
sleep 2
dump_frames "$LOG/play.csv" 10 dmp_play_probe
wait $PLAY 2>/dev/null
PLAYF=$(cut -d, -f2 "$LOG/play.csv" 2>/dev/null | wc -l)
chk "回放出来的每一帧都能回到 bag 里逐字段相同" "$(win_cmp "$LOG/play.csv" "$LOG/bag.csv" play-vs-bag)" "1"
echo "  回放侧 $PLAYF 帧; play 日志尾部: $(grep -o 'duration:[^ ]*' "$LOG/play.log" 2>/dev/null | head -1)"

echo "=== [6] diagnostic_aggregator(标准诊断栈聚合) ==="
if ros2 pkg prefix diagnostic_aggregator >/dev/null 2>&1; then
  echo "  包在, 本格待补(本轮未实现聚合断言, 不冒充)"
  SKIP=$((SKIP+1))
else
  echo "  SKIP: 本机无 ros-humble-diagnostic-aggregator 且 sudo 需要密码, 装不了。"
  echo "  要跑这格请先在 VM 上: sudo apt install ros-humble-diagnostic-aggregator"
  echo "  (apt-cache 有候选版本, 说明只是没装, 不是源里没有)"
  SKIP=$((SKIP+1))
fi

echo "=== [9] 汇总 ==="
echo "  PASS=$PASS FAIL=$FAIL SKIP=$SKIP PRED_OVERTURNED=$PRED_FAIL"
[ "$SKIP" -gt 0 ] && echo "  注: SKIP 不算通过。本脚本对『告警沿标准诊断栈传播』这件事**没有任何证据**, 只有 bag 那几格。"
[ "$PRED_FAIL" -gt 0 ] && echo "  注: PRED_OVERTURNED 不是缺陷, 是预设被现实推翻 —— 必须写进文档, 不许把期望改成实测。"
if [ "$FAIL" = 0 ] && [ "$PASS" -gt 0 ]; then
  if [ "$PRED_FAIL" -gt 0 ]; then echo "ALL_DOWNSTREAM_BAG_ASSERTS_PASS  (注: 另有 $PRED_FAIL 格预设被推翻, 见上面 PRED-OVERTURNED 行)"; else echo "ALL_DOWNSTREAM_BAG_ASSERTS_PASS"; fi
fi
echo SCRIPT_DONE

# 收尾: 别把进程留给下一轮(素材录 B-08: pkill 的模式串会匹配到自己)
kill -INT $LAUNCH 2>/dev/null; kill $GW $SIM 2>/dev/null
exit 0
