#!/usr/bin/env bash
# vm_aggregator_probe.sh — 一次性侦查: diagnostic_aggregator 装上之后, 它的**真实**接口长什么样
#
# 为什么先侦查再写断言: [6] 那格要断言的是"标准诊断栈确实聚合了我们的 /diagnostics"。
# 需要知道三件事才写得出可证伪的断言: (i) 聚合输出话题叫什么; (ii) 参数文件里节点名那一级
# 到底填什么(example_analyzers.yaml 顶层是 `analyzers:`, 但节点名是不是它, 不看源码不知道);
# (iii) GenericAnalyzer 的 startswith/contains 匹的是 DiagnosticStatus.name 的哪种形状
# (我们的 name 是 telemetry_link / Temp_ch0, 而 example 里写的是 '/arms' 带前导斜杠)。
# 这三件都是**凭印象就会写错**的东西(素材录 B-04/B-11 那一类: 记成"应该是这样"的 API)。
#
# 本脚本不作断言, 只打印读数; 结论不进验收表。
set +u
set +e
source /opt/ros/humble/setup.bash
DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
cd "$DMP_HOME/dmp" || exit 1
source install/setup.bash
LOG=/tmp/dmp_agg_probe
rm -rf "$LOG"; mkdir -p "$LOG"

pkill -f "$PWD/build_linux/device_simulator" 2>/dev/null
pkill -f "$PWD/build_linux/gateway_service"  2>/dev/null
pkill -f bridge_node 2>/dev/null
pkill -f aggregator_node 2>/dev/null
pkill -f example_pub.py 2>/dev/null
sleep 1

echo "=== [A] 发行版自带的 example 能不能在这台 VM 上跑起来(厂商配置做基准) ==="
nohup ros2 run diagnostic_aggregator aggregator_node --ros-args \
      --params-file /opt/ros/humble/share/diagnostic_aggregator/example_analyzers.yaml \
      > "$LOG/agg_example.log" 2>&1 & AGGA=$!
sleep 3
nohup ros2 run diagnostic_aggregator example_pub.py > "$LOG/example_pub.log" 2>&1 & PUBA=$!
sleep 6
echo "--- aggregator 日志(头 12 行) ---"; head -12 "$LOG/agg_example.log"
echo "--- 节点名 ---"; timeout 20 ros2 node list
echo "--- 话题(含诊断关键字) ---"; timeout 20 ros2 topic list | grep -Ei 'diag|agg'
echo "--- 聚合话题类型 ---"
AGGT=$(timeout 20 ros2 topic list | grep -Ei 'agg' | head -1)
echo "  AGGT=$AGGT"
[ -n "$AGGT" ] && timeout 25 ros2 topic echo --once "$AGGT" > "$LOG/agg_example_echo.txt" 2>&1
echo "--- 聚合内容(example, 前 40 行) ---"; head -40 "$LOG/agg_example_echo.txt"
kill $PUBA $AGGA 2>/dev/null
sleep 2

echo "=== [B] 换成我们自己的 /diagnostics ==="
nohup ./build_linux/device_simulator 9000 > "$LOG/sim.log" 2>&1 & SIM=$!
sleep 1
nohup ./build_linux/gateway_service 9100 127.0.0.1 9000 > "$LOG/gw.log" 2>&1 & GW=$!
sleep 2
nohup ros2 launch dmp_ros2_bridge dmp_bridge.launch.py device_id:=bed01 gateway_port:=9100 \
      > "$LOG/bridge.log" 2>&1 &
sleep 12
echo "--- 桥生命周期 ---"; timeout 20 ros2 lifecycle get /dmp_bridge_bed01
echo "--- 我们的 /diagnostics 原文(取一条) ---"
timeout 25 ros2 topic echo --once /diagnostics > "$LOG/ours.txt" 2>&1
head -45 "$LOG/ours.txt"

cat > "$LOG/agg_ours.yaml" <<'YAML'
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
echo "--- 参数文件(我们写的) ---"; cat "$LOG/agg_ours.yaml"
nohup ros2 run diagnostic_aggregator aggregator_node --ros-args \
      --params-file "$LOG/agg_ours.yaml" > "$LOG/agg_ours.log" 2>&1 & AGGB=$!
sleep 8
echo "--- aggregator 日志(头 20 行) ---"; head -20 "$LOG/agg_ours.log"
echo "--- 节点名 ---"; timeout 20 ros2 node list
echo "--- 话题 ---"; timeout 20 ros2 topic list | grep -Ei 'diag|agg'
AGGT2=$(timeout 20 ros2 topic list | grep -Ei 'agg' | head -1)
echo "  AGGT2=$AGGT2"
[ -n "$AGGT2" ] && timeout 25 ros2 topic echo --once "$AGGT2" > "$LOG/agg_ours_echo.txt" 2>&1
echo "--- 聚合内容(我们自己的, 前 80 行) ---"; head -80 "$LOG/agg_ours_echo.txt"
echo "--- 聚合项统计 ---"
python3 - <<'PY' 2>&1
import re
t = open('/tmp/dmp_agg_probe/agg_ours_echo.txt').read() if __import__('os').path.exists('/tmp/dmp_agg_probe/agg_ours_echo.txt') else ''
names = re.findall(r'^- name: (.*)$', t, re.M)
print('status 条数=%d' % len(names))
for n in names[:20]:
    print('  name=%s' % n)
print('有 hardware_id 行数=%d' % len(re.findall(r'hardware_id:', t)))
print('level 取值分布=%s' % (re.findall(r'^      level: (\d+)$', t, re.M) or re.findall(r'level: (\d+)', t)))
PY

echo "=== [C] 收尾 ==="
kill $GW $SIM 2>/dev/null
pkill -f aggregator_node 2>/dev/null
pkill -f dmp_bridge.launch.py 2>/dev/null
pkill -f bridge_node 2>/dev/null
sleep 2
echo "  残留: agg=$(pgrep -f aggregator_node | wc -l) bridge=$(pgrep -f 'bridge_node|dmp_bridge.launch.py' | wc -l) simgw=$(pgrep -f 'build_linux/(device_simulator|gateway_service)' | wc -l)"
echo PROBE_DONE
