#!/usr/bin/env bash
# wsl_setup.sh — 在 WSL2(Ubuntu) 里安装 gRPC 依赖并构建/自检真 gRPC 服务层
#
# 用法(在 WSL2 终端):
#   cd /mnt/e/Work/McuProject/DeviceMonitorPlatform
#   bash wsl_setup.sh            # 装依赖(会 sudo) + 构建 + 合成模式冒烟自检
#   bash wsl_setup.sh --build    # 只构建, 跳过 apt 安装
#   bash wsl_setup.sh --run      # 构建后前台启动 server(手动 Ctrl+C 停)
set -euo pipefail
cd "$(dirname "$0")"

SKIP_INSTALL=0; RUN_FOREGROUND=0
for a in "$@"; do
  case "$a" in
    --build) SKIP_INSTALL=1 ;;
    --run)   RUN_FOREGROUND=1 ;;
  esac
done

if [ "$SKIP_INSTALL" -eq 0 ]; then
  echo "[1/4] 安装依赖 (apt, 需要 sudo)..."
  sudo apt-get update
  sudo apt-get install -y build-essential cmake pkg-config \
       libgrpc++-dev protobuf-compiler-grpc protobuf-compiler libprotobuf-dev
fi

echo "[2/4] 配置 CMake (Linux 构建目录 build_wsl)..."
# 清掉上一次失败配置留下的缓存, 避免陈旧 find_package 结果干扰
rm -rf build_wsl
# 预检: apt 版 gRPC 通过 pkg-config 暴露(无 gRPCConfig.cmake), 缺失则先报清晰错误
if ! pkg-config --exists grpc++; then
  echo "  [!] pkg-config 找不到 grpc++。请确认已装: sudo apt-get install -y libgrpc++-dev protobuf-compiler-grpc pkg-config"
  echo "      pkg-config --list-all | grep -i grpc" >&2
  exit 1
fi
echo "  依赖版本: grpc++=$(pkg-config --modversion grpc++)  protobuf=$(pkg-config --modversion protobuf)"
# 说明: /mnt/e 是 DrvFs, 编译较慢但可用。protoc/grpc_cpp_plugin 由上一步的包提供。
cmake -S service -B build_wsl -DCMAKE_BUILD_TYPE=Release

echo "[3/4] 构建 dmp_grpc_server + grpc_client_probe ..."
cmake --build build_wsl -j"$(nproc)"

if [ "$RUN_FOREGROUND" -eq 1 ]; then
  echo "[run] 前台启动合成模式 server (:50051), Ctrl+C 停止 ..."
  exec ./build_wsl/dmp_grpc_server 50051
fi

echo "[4/4] 冒烟自检: 后台起合成模式 server, 用 probe 连它跑 GetStats/GetAlarms/Subscribe ..."
./build_wsl/dmp_grpc_server 50051 >/tmp/dmp_grpc.log 2>&1 &
SRV=$!
sleep 1.5
until grep -q "listening" /tmp/dmp_grpc.log 2>/dev/null; do sleep 0.3; done
./build_wsl/grpc_client_probe 127.0.0.1:50051 10 || { echo "probe 失败"; cat /tmp/dmp_grpc.log; kill $SRV 2>/dev/null; exit 1; }
kill $SRV 2>/dev/null || true
echo "自检完成。详见 /tmp/dmp_grpc.log"
