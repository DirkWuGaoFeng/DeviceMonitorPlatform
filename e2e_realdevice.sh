#!/usr/bin/env bash
# e2e_realdevice.sh — 真机全链路: STM32(串口) -> Windows gateway(:9100) -> WSL gRPC(:50051) -> probe
# 用法: bash e2e_realdevice.sh <windows_ip>   (缺省 172.22.176.1)
set -u
WIN_IP="${1:-172.22.176.1}"
# 兼容直接执行与管道执行(tr -d 后喂给 bash): $0 可能不是脚本路径
[ -d build_wsl ] || cd /mnt/e/Work/McuProject/DeviceMonitorPlatform

echo "[1/3] 启动 dmp_grpc_server :50051  upstream=$WIN_IP:9100 (上游模式+RAW 订阅)"
./build_wsl/dmp_grpc_server 50051 "$WIN_IP" 9100 >/tmp/dmp_e2e.log 2>&1 &
SRV=$!
sleep 1.5
until grep -q "listening" /tmp/dmp_e2e.log 2>/dev/null; do
  sleep 0.3
  kill -0 $SRV 2>/dev/null || { echo "server 启动失败:"; cat /tmp/dmp_e2e.log; exit 1; }
done
grep listening /tmp/dmp_e2e.log

echo "[2/3] 等 6 秒让真机帧流灌进来 (设备侧 ~5帧/秒)"
sleep 6

echo "[3/3] probe: GetStats x2 + GetAlarms + Subscribe"
./build_wsl/grpc_client_probe 127.0.0.1:50051 10
RC=$?
kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
echo "---- server log ----"
cat /tmp/dmp_e2e.log
exit $RC
