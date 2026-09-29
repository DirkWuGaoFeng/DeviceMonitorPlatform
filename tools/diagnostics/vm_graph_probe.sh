#!/usr/bin/env bash
# 取证探针: [1] 那条 "unconfigured 时 /dmp/frames 不在 topic list" 的 FAIL,
# 读数是来自现场(真有参与者), 还是来自 ros2 daemon 的图缓存?
#
# 设计要点:
#   1) 不碰 /dmp/frames —— 用独立话题名, 免得探针自己变成污染源。
#   2) 两条退出路径都测: SIGTERM(rclcpp 有机会道别) 与 SIGKILL(没机会道别)。
#      脚本里的 pkill 默认发 TERM, 但单线程容器被 21s connect 占满时,
#      信号处理器要等回调返回才跑得动 —— 那一次退出在效果上等于没道别。
#   3) 每次都同时读两路: 走 daemon 的 `ros2 topic list` 与不走 daemon 的
#      `ros2 topic list --no-daemon`, 两者不一致就说明断言读的不是现场。
#
# 两条防自己量错的约束 (对应复盘录 B-35/B-36):
#   a) 存活判定用 kill -0 直接问那个 pid, 不用 pgrep -f 去猜命令行;
#      发布进程由 bash 直接后台起, 不套 timeout —— 套了以后 kill 打的是包装进程,
#      真发布会变孤儿, "我已经把它杀了"这句就成了假的。
#   b) 第 1 步是探针的自检: 发布者在场时两个读数必须都是 1。
#      这一步不通就说明探针本身没在测它声称在测的东西, 后面全部作废。
# shell 检查顺序不能反: ament 的 setup.bash 内部会读未定义变量, set -u 在它前面会把 source 打断
# (主脚本 vm_lifecycle_compose.sh 同样靠 set +u 过这一关)。
source /opt/ros/humble/setup.bash
set +u
TOPIC=/dmp_probe_frame

cnt()       { timeout 20 ros2 topic list 2>/dev/null | grep -c "^${TOPIC}\$"; }
cnt_fresh() { timeout 30 ros2 topic list --no-daemon --spin-time 5 2>/dev/null | grep -c "^${TOPIC}\$"; }
alive()     { kill -0 "$1" 2>/dev/null && echo 1 || echo 0; }

# pub: 直接后台起 python, 打印它自己的 pid。
# 注意那个 >/dev/null: 命令替换 $(pub) 会一直读到管道 EOF 才返回,
# 而后台进程继承了这根管道 —— 不重定向的话 $(pub 300) 会阻塞 300 秒,
# 看起来像探针卡死。这是"我调用它的方式"给"它本身"加的坑。
pub() {
  python3 - "$1" >/dev/null 2>&1 <<'PY' &
import sys, time, rclpy
from rclpy.node import Node
from std_msgs.msg import String
dur = float(sys.argv[1])
rclpy.init(); n = Node('probe_pub')
p = n.create_publisher(String, '/dmp_probe_frame', 10)
t0 = time.time()
while time.time() - t0 < dur:
    p.publish(String()); rclpy.spin_once(n, timeout_sec=0.1)
PY
  local pid=$!
  disown "$pid" 2>/dev/null || true
  echo "$pid"
}

echo "=== 0) 现场基线: 什么都没跑 ==="
echo "  daemon 路 = $(cnt)   新发现路 = $(cnt_fresh)"

echo "=== 1) 自检: 发布者在场, 两路读数都必须是 1 ==="
P=$(pub 300)
if [ -z "${P:-}" ] || ! kill -0 "$P" 2>/dev/null; then
  echo "  PROBE_ABORT: 发布者 pid='${P:-空}' 不存在 —— 下面的读数全部不作数"
  exit 2
fi
sleep 8
echo "  pid=$P 存活=$(alive "$P")  进程行=$(ps -o args= -p "$P" 2>/dev/null | cut -c1-40)"
echo "  daemon 路 = $(cnt)   新发现路 = $(cnt_fresh)"

echo "=== 2) SIGTERM (优雅退出, 有机会道别) ==="
kill -TERM "$P" 2>/dev/null; sleep 4
echo "  存活=$(alive "$P")"
echo "  daemon 路 = $(cnt)   新发现路 = $(cnt_fresh)"

echo "=== 3) SIGKILL (没机会道别) ==="
P2=$(pub 300)
if [ -z "${P2:-}" ] || ! kill -0 "$P2" 2>/dev/null; then
  echo "  PROBE_ABORT: 第二个发布者 pid='${P2:-空}' 不存在"
  exit 2
fi
sleep 8
echo "  pid=$P2 存活=$(alive "$P2")"
kill -KILL "$P2" 2>/dev/null; sleep 4
echo "  存活=$(alive "$P2")"
echo "  daemon 路 = $(cnt)   新发现路 = $(cnt_fresh)"

echo "=== 4) 停掉 daemon 再读 (若 daemon 路随后归零, 缓存就是唯一来源) ==="
timeout 20 ros2 daemon stop >/dev/null 2>&1; sleep 2
echo "  daemon 路 = $(cnt)   新发现路 = $(cnt_fresh)"
timeout 20 ros2 daemon start >/dev/null 2>&1

echo "=== 5) 关键一环: 回调被占住时发 TERM, 它跟 KILL 有区别吗 ==="
# 这一步把"验收脚本用 pkill(默认 TERM) 杀容器"与"毒化格里容器正被 21s connect 占满"
# 这两个事实接起来: 信号处理器要等回调返回才跑得动, 所以 TERM 在那一刻发不出去。
# 预期: 节点最终会自己退出(道别没赶上), daemon 路仍报 1, 新发现路报 0 —— 与 KILL 同果。
python3 - 25 >/dev/null 2>&1 <<'PY' &
import sys, time, rclpy
from rclpy.node import Node
from std_msgs.msg import String
block = float(sys.argv[1])
rclpy.init(); n = Node('probe_blocked')
p = n.create_publisher(String, '/dmp_probe_frame', 10)
t0 = time.time(); fired = [False]
def cb():
    if fired[0]:
        return
    fired[0] = True
    p.publish(String())
    time.sleep(block)          # 模拟一个长回调: 期间执行器不处理任何信号/其它回调
n.create_timer(0.5, cb)
while time.time() - t0 < 200 and rclpy.ok():
    rclpy.spin_once(n, timeout_sec=0.1)
PY
BP=$!
sleep 8
echo "  发送前: 存活=$(alive "$BP")  daemon 路 = $(cnt)  新发现路 = $(cnt_fresh)"
kill -TERM "$BP" 2>/dev/null
sleep 4
echo "  TERM 后 4s(回调还占着): 存活=$(alive "$BP")  daemon 路 = $(cnt)  新发现路 = $(cnt_fresh)"
sleep 30
echo "  回调返回、进程已退出后: 存活=$(alive "$BP")  daemon 路 = $(cnt)  新发现路 = $(cnt_fresh)"
echo "PROBE_DONE"
