#!/usr/bin/env bash
# vm_grpc_build_run.sh — 在原生 Linux(VM Ubuntu 22.04) 上构建并自检验真 gRPC 服务层
#
# 为什么存在: 项目把 WSL2 从环境清单退役后, Linux 侧只剩"原生 Ubuntu 22.04"一套环境
#   (VM 为验证载体, 实体工控机/厂商 BSP 为交付形态)。原先只在 WSL 里跑的 dmp_grpc_server
#   必须能在这套原生 Linux 上开箱构建 + 跑通, 才谈得上"gRPC 服务层已迁至原生 Linux 做实"。
#   本脚本就是那条可复现的验收链 —— 没有它, 文档里那句"迁好了"就只是"我说了算"。
#
# 设计约束(不是讲究, 是踩过的坑):
#   - 不用 set -e: 本脚本的价值是把每一步真实结果都打出来, 中途静默退出=交出假绿报告。
#   - 结论行必须是 `PASS=<数字>`: run_vm_rounds.ps1 的守卫只认这一行(见其注释: 光有文件不等于
#     有结论)。没有断言结论就退, 会被当"跑过了" —— 那正是要拦的情况。
#   - grpc_server 的合成模式不依赖任何外部进程/跨主机网络, 所以这一档在 VM 上天然自洽;
#     RAW 上游那档(Assertion B)需要一个已构建的 Linux 网关, 缺就明确 SKIP, 不谎报。
#
# 用法(通常在仓库根, 由 run_vm_rounds.ps1 同步后驱动):
#   bash tools/vm_grpc_build_run.sh                 # 装依赖(若无)+构建+合成模式冒烟(核心判据)
#   INSTALL=1 bash tools/vm_grpc_build_run.sh       # 强制先 apt 装 gRPC/protobuf(首次上机)
#   GRPC_PORT=50081 bash tools/vm_grpc_build_run.sh # 换端口(避开占用)
#   GATEWAY=<host:port> bash tools/vm_grpc_build_run.sh  # 追加 RAW 上游订阅档(需网关已在跑)
set +u
set +e

REPO="${REPO:-$(cd "$(dirname "$0")/.." && pwd)}"
cd "$REPO" || { echo "FAIL: 仓库根不存在: $REPO"; echo "PASS=0"; echo "SCRIPT_DONE"; exit 1; }
BUILD_DIR="${BUILD_DIR:-build_linux_grpc}"
PORT="${GRPC_PORT:-50071}"
LOG_DIR="${LOG_DIR:-/tmp}"
TOTAL=0
PASSED=0

echo "=== [0] 环境 (本轮现算, 不引用历史) ==="
echo "  UNAME=$(uname -sr)"
echo "  HEAD=$(git rev-parse --short HEAD 2>/dev/null) branch=$(git rev-parse --abbrev-ref HEAD 2>/dev/null)"
echo "  grpc++=$(pkg-config --modversion grpc++ 2>/dev/null || echo MISSING) protobuf=$(pkg-config --modversion protobuf 2>/dev/null || echo MISSING)"

echo "=== [1] 依赖 (apt 版 gRPC 只发 pkg-config, 无 gRPCConfig.cmake) ==="
if [ "${INSTALL:-0}" -eq 1 ] || ! pkg-config --exists grpc++; then
  echo "  安装: build-essential cmake pkg-config libgrpc++-dev protobuf-compiler-grpc ..."
  sudo apt-get update >/dev/null 2>&1
  sudo apt-get install -y build-essential cmake pkg-config \
       libgrpc++-dev protobuf-compiler-grpc protobuf-compiler libprotobuf-dev 2>&1 | tail -3
fi
if ! pkg-config --exists grpc++; then
  echo "  ASSERT[deps]=FAIL  (pkg-config 找不到 grpc++; 装 libgrpc++-dev protobuf-compiler-grpc 后重跑)"
  echo "PASS=0 TOTAL=1"; echo "SCRIPT_DONE"; exit 1
fi
echo "  ASSERT[deps]=PASS"

echo "=== [2] 构建 dmp_grpc_server + grpc_client_probe (cmake -S service) ==="
rm -rf "$BUILD_DIR"
cmake -S service -B "$BUILD_DIR" -DCMAKE_BUILD_TYPE=Release 2>&1 | tail -4
cmake --build "$BUILD_DIR" -j"$(nproc)" 2>&1 | tail -6
BUILD_EXIT=$?
SRV="$BUILD_DIR/dmp_grpc_server"; PRB="$BUILD_DIR/grpc_client_probe"
if [ "$BUILD_EXIT" -ne 0 ] || [ ! -x "$SRV" ] || [ ! -x "$PRB" ]; then
  echo "  ASSERT[build]=FAIL (build_exit=$BUILD_EXIT srv=$([ -x "$SRV" ] && echo yes || echo no) probe=$([ -x "$PRB" ] && echo yes || echo no))"
  echo "PASS=0 TOTAL=1"; echo "SCRIPT_DONE"; exit 1
fi
echo "  ASSERT[build]=PASS"

echo "=== [3] 合成模式端到端冒烟: server(:$PORT) <- probe GetStats/GetAlarms/Subscribe ==="
TOTAL=$((TOTAL+1))
SLOG="$LOG_DIR/dmp_grpc_srv_$PORT.log"
"$SRV" "$PORT" >"$SLOG" 2>&1 &
SRV_PID=$!
# 就绪: 等到 server 打出 listening, 最多 ~10s
for _ in $(seq 1 30); do grep -q "listening on" "$SLOG" 2>/dev/null && break; sleep 0.3; done
if ! grep -q "listening on" "$SLOG" 2>/dev/null; then
  echo "  ASSERT[synthetic]=FAIL (server 未 listening, 尾日志:)"; tail -5 "$SLOG"
  kill "$SRV_PID" 2>/dev/null
else
  PLOG="$LOG_DIR/dmp_grpc_probe_$PORT.log"
  "$PRB" "127.0.0.1:$PORT" 10 >"$PLOG" 2>&1
  PRB_RC=$?
  NSAMP=$(grep -c "SAMPLE ts=" "$PLOG" 2>/dev/null)
  CRC=$(sed -n 's/.*crc_err=\([0-9]*\).*/\1/p' "$PLOG" | head -1)
  DONE=$(grep -c "probe done." "$PLOG" 2>/dev/null)
  if [ "$PRB_RC" -eq 0 ] && [ "$DONE" -ge 1 ] && [ "$NSAMP" -ge 10 ] && [ "${CRC:-1}" -eq 0 ]; then
    echo "  ASSERT[synthetic]=PASS  (probe_rc=0 samples=$NSAMP crc_err=$CRC)"; PASSED=$((PASSED+1))
  else
    echo "  ASSERT[synthetic]=FAIL  (probe_rc=$PRB_RC done=$DONE samples=$NSAMP crc_err=${CRC:-NA})"
    echo "  ---- probe 尾 ----"; tail -6 "$PLOG"; echo "  ---- server 尾 ----"; tail -4 "$SLOG"
  fi
  kill "$SRV_PID" 2>/dev/null
fi

echo "=== [4] (可选) RAW 上游订阅档: server 连已在跑的网关, 证 grpc_server 是网关的独立 RAW 订阅者 ==="
if [ -n "${GATEWAY:-}" ]; then
  TOTAL=$((TOTAL+1))
  GH="${GATEWAY%%:*}"; GP="${GATEWAY##*:}"
  "$SRV" "$((PORT+1))" "$GH" "$GP" >"$LOG_DIR/dmp_grpc_up_$PORT.log" 2>&1 &
  UP_PID=$!
  for _ in $(seq 1 30); do grep -q "listening on" "$LOG_DIR/dmp_grpc_up_$PORT.log" 2>/dev/null && break; sleep 0.3; done
  sleep 1.5
  "$PRB" "127.0.0.1:$((PORT+1))" 5 >"$LOG_DIR/dmp_grpc_upprobe_$PORT.log" 2>&1
  URC=$?
  UCRC=$(sed -n 's/.*crc_err=\([0-9]*\).*/\1/p' "$LOG_DIR/dmp_grpc_upprobe_$PORT.log" | head -1)
  if [ "$URC" -eq 0 ] && [ "${UCRC:-1}" -eq 0 ]; then
    echo "  ASSERT[raw_upstream]=PASS  (订阅 $GATEWAY 经 gRPC 转发, crc_err=$UCRC)"; PASSED=$((PASSED+1))
  else
    echo "  ASSERT[raw_upstream]=FAIL  (rc=$URC crc_err=${UCRC:-NA} —— 网关 $GATEWAY 是否在跑/可达?)"
  fi
  kill "$UP_PID" 2>/dev/null
else
  echo "  ASSERT[raw_upstream]=SKIP  (未给 GATEWAY=host:port; 合成档已足以证 gRPC 迁至原生 Linux 做实)"
fi

echo "=== [5] 结论 ==="
echo "  PASS=$PASSED TOTAL=$TOTAL"
echo "SCRIPT_DONE"
[ "$PASSED" -ge 1 ] && [ "$PASSED" -eq "$TOTAL" ] && exit 0 || exit 1
