#!/usr/bin/env bash
# vm_native_build.sh — 在 Linux(VM/CI) 侧从 git bundle 同步并做原生构建 + ctest
#
# 用法:
#   1) Windows 侧打包:  git bundle create ../build_tmp/dmp.bundle main
#   2) 拷贝到目标机:    scp dmp.bundle dmp_vm_native_build.sh user@host:~/
#   3) 目标机执行:      bash ~/dmp_vm_native_build.sh
#
# 为什么用 bundle 而不是 git clone: 仓库尚未公开可见时也能同步完整历史,
# 且不要求目标机能访问 GitHub; bundle 自带完整性校验 (git bundle verify)。
#
# 注意: 不用 set -e —— 本脚本的价值在于把每一步的真实结果都打出来。
set +u
set +e

DMP_HOME="${DMP_HOME:-$HOME/RosProject}"
REPO="$DMP_HOME/dmp"
BUNDLE="${BUNDLE:-$HOME/dmp.bundle}"

echo "=== [1] bundle ==="
ls -l "$BUNDLE" 2>&1 | tail -1

echo "=== [2] sync repo ==="
if [ -d "$REPO/.git" ]; then
  echo "mode=fetch (已存在, 只做快进, 不覆盖任何本地文件)"
  cd "$REPO" || exit 1
  git fetch -q "$BUNDLE" main && git merge --ff-only FETCH_HEAD 2>&1 | tail -3
else
  echo "mode=clone"
  mkdir -p "$DMP_HOME"
  git clone -q -b main "$BUNDLE" "$REPO" && echo "CLONE=ok" || echo "CLONE=fail"
  cd "$REPO" || exit 1
fi
echo "HEAD=$(git rev-parse --short HEAD) branch=$(git rev-parse --abbrev-ref HEAD)"
git log --oneline -3

echo "=== [3] native cmake build ==="
cmake -S . -B build_linux -DCMAKE_BUILD_TYPE=Debug 2>&1 | tail -4
cmake --build build_linux -j"$(nproc)" 2>&1 | tail -6
echo "build_exit=$?"

echo "=== [4] ctest ==="
(cd build_linux && ctest --output-on-failure 2>&1 | tail -12)

echo "=== [5] assertion counts (以程序打印为准, 不看源码 CHECK 行数) ==="
for t in test_protocol test_storage test_frame_parity test_pipeline test_service_proto; do
  p="build_linux/$t"
  [ -x "$p" ] || continue
  echo "---- $t: $("$p" 2>&1 | tail -1)"
done
echo "SCRIPT_DONE"
