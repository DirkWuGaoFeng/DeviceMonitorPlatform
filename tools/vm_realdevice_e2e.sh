#!/usr/bin/env bash
# vm_realdevice_e2e.sh — 跨机真机验收: STM32(USB 串口) -> Windows 网关 -> VM 内 ROS2 话题
#
# 前置条件(都在 Windows 侧, 本脚本无法代劳):
#   1) 板子上电、固件在跑演示任务, 设备管理器里能看到 STLink Virtual COM Port
#   2) 起网关: build_ci_win\gateway_service.exe 9100 --serial COM4
#   3) VM 已 colcon build 过本仓库
#
# 用法(在 VM 上): bash tools/vm_realdevice_e2e.sh [网关IP] [端口] [device_id]
#   默认 192.168.109.1 9100 bed01
#
# 服务名真名是 /dmp/<device_id>/set_rule (带设备段, 因为多设备=多实例),
# 话题才只按 topic_prefix 走 (/dmp/frames)。别把两者写成同一个前缀 —— 会卡在
# "waiting for service to become available" 而看不到任何错误。
#
# 归因顺序: 先看网关, 再看桥。裸探测不涉及 ROS, 零数据时能立刻排除 DDS 变量。
# 注意别用 `set -euo pipefail`: ament 的 setup.bash 内部会引用未定义变量(如 AMENT_PYTHON_EXECUTABLE
# 在不同发行版下有无不确定), 在 set -u 下 source 会直接终止脚本 —— 现象是"环境加载失败", 而非报错行。
set +u
GW_HOST="${1:-192.168.109.1}"
GW_PORT="${2:-9100}"
DEV="${3:-bed01}"
SVC="/dmp/$DEV"
LOG=/tmp/dmp_realdevice
rm -rf "$LOG"; mkdir -p "$LOG"

echo "=== [1/5] 裸 TCP 看网关是否在出真机帧(不经 ROS) ==="
python3 - "$GW_HOST" "$GW_PORT" <<'PY'
import socket, sys, time
host, port = sys.argv[1], int(sys.argv[2])
s = socket.create_connection((host, port), timeout=3)
s.sendall(b"SUBSCRIBE\n"); time.sleep(3.0)
s.sendall(b"STATS\n"); s.settimeout(2.0)
buf = b""
try:
    while True:
        b = s.recv(4096)
        if not b: break
        buf += b
        if b"STATS ok=" in buf and buf.endswith(b"\n"): break
except socket.timeout:
    pass
s.close()
text = buf.decode("utf-8", "replace")
seqs = [int(l.split("seq=")[1].split()[0]) for l in text.splitlines() if "seq=" in l]
stats = [l for l in text.splitlines() if l.startswith("STATS ")]
print(f"  frames_3s={len(seqs)} seq=[{seqs[0] if seqs else '-'}..{seqs[-1] if seqs else '-'}] {stats}")
print("  VERDICT: " + ("真机在出帧" if len(seqs) > 5 else "网关没有帧 —— 检查串口/板子, 后面的步骤无意义"))
PY

echo "=== [2/5] 起桥(跨机连 $GW_HOST:$GW_PORT) ==="
source /opt/ros/humble/setup.bash
source "$HOME/RosProject/dmp/install/setup.bash"
if ! ros2 pkg prefix dmp_ros2_bridge >/dev/null 2>&1; then
    echo "  FAIL: dmp_ros2_bridge 不在环境里 —— 先跑 tools/vm_ros2_build.sh"; exit 2
fi
ros2 launch dmp_ros2_bridge dmp_bridge.launch.py \
    device_id:="$DEV" gateway_host:="$GW_HOST" gateway_port:="$GW_PORT" \
    > "$LOG/bridge.log" 2>&1 &
BRIDGE_PID=$!
sleep 10
echo "  bridge pid=$BRIDGE_PID alive=$(kill -0 $BRIDGE_PID 2>/dev/null && echo yes || echo no)"

echo "=== [3/5] 采话题(计时一律用 timeout -s INT: SIGTERM 会丢块缓冲, 假造 0 条) ==="
timeout -s INT 8 ros2 topic echo /dmp/frames > "$LOG/frames.txt" 2>&1
timeout -s INT 6 ros2 topic echo /diagnostics > "$LOG/diag.txt" 2>&1
echo "  frames: seq_lines=$(grep -c 'seq:' "$LOG/frames.txt")"
# DiagnosticArray 的 YAML 是 `- key: decoded_ok` 换行 `value: '84'`, 所以取值必须 -A1。
# 桥侧 /diagnostics 只发 decoded_ok / crc_err 两个键(dropped 在网关侧计数, 由 selftest 回读)。
# 去探测一个不存在的键会得到 "?", 看上去像故障, 实际是验收脚本自己的错。
for k in decoded_ok crc_err; do
    v=$(grep -A1 "key: $k" "$LOG/diag.txt" | grep -m1 'value:' | sed "s/.*value: '//; s/'//")
    printf '    %-10s = %s\n' "$k" "${v:-?}"
done
grep -m1 'message: ' "$LOG/diag.txt" | sed 's/^/    /'
grep -m4 -E "name: '(TEMP|HR|SPO2|CONC)'|value:" "$LOG/frames.txt" | sed 's/^/    /' | head -8
echo "  QoS 实测(桥侧发布端):"
ros2 topic info /dmp/frames --verbose 2>/dev/null | grep -A1 -E "type|Reliability" | sed 's/^/    /' | head -12

echo "=== [4/5] 下行写阈值: 桥 set_rule -> 网关 RULES 独立回读 ==="
timeout -s INT 20 ros2 service call $SVC/set_rule dmp_msgs/srv/SetRule \
    "{kind: 2, low: 40.0, high: 105.0, message: 'realdevice test'}" > "$LOG/setrule.txt" 2>&1
echo "  set_rule: $(grep -m1 'accepted=' "$LOG/setrule.txt")"
timeout -s INT 20 ros2 service call $SVC/get_rules dmp_msgs/srv/GetRules > "$LOG/getrules.txt" 2>&1
# 服答在 echo 里是 Python repr 单行(Rule(kind=1, ...)), 不是 YAML, 所以不能 grep 'low:'。
echo "  get_rules: 条数=$(grep -o 'Rule(' "$LOG/getrules.txt" | wc -l)  HR当前区间=$(grep -o "kind_name='HR', low=[0-9.]*, high=[0-9.]*" "$LOG/getrules.txt" | head -1)"
python3 - "$GW_HOST" "$GW_PORT" <<'PY'
import socket, sys
s = socket.create_connection((sys.argv[1], int(sys.argv[2])), timeout=3)
s.sendall(b"RULES\n"); s.settimeout(2.0)
buf = b""
try:
    while True:
        b = s.recv(4096)
        if not b: break
        buf += b
except socket.timeout:
    pass   # 靠超时收尾: 不能在第一个 \n 就 break, RULES 是多行应答, 那样只会拿到第一条
s.close()
for line in buf.decode("utf-8", "replace").splitlines():
    if line.startswith("RULE "):
        print("  网关侧独立回读(证明写进的是共享判定引擎, 不是桥的本地状态): " + line.strip())
PY

echo "=== [5/5] 非法写入必须被拒(区间倒置) + 自检 ==="
timeout -s INT 15 ros2 service call $SVC/set_rule dmp_msgs/srv/SetRule \
    "{kind: 2, low: 120.0, high: 30.0, message: 'inverted'}" > "$LOG/bad.txt" 2>&1
echo "  inverted: $(grep -m1 -E 'accepted|reason' "$LOG/bad.txt" | tr -s ' ')"
timeout -s INT 15 ros2 service call $SVC/selftest dmp_msgs/srv/Selftest > "$LOG/selftest.txt" 2>&1
echo "  selftest: $(grep -m1 -E 'rule_count|ok' "$LOG/selftest.txt" | tr -s ' ')"

# 恢复现场必须在杀桥之前: 服务提供方就是桥自己, 先 kill 则这句只能得到超时。
timeout -s INT 20 ros2 service call $SVC/set_rule dmp_msgs/srv/SetRule \
    "{kind: 2, low: 50.0, high: 110.0, message: 'HR out of range'}" > "$LOG/restore.txt" 2>&1
echo "=== 已恢复 HR 默认区间: $(grep -m1 'accepted=' "$LOG/restore.txt") ==="

kill $BRIDGE_PID 2>/dev/null
pkill -f bridge_node 2>/dev/null
echo "=== 结束, 日志在 $LOG ==="
