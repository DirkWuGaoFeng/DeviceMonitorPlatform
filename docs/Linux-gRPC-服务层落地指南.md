# Linux 落地真 gRPC 服务层 —— 操作指南

> 目标：把 `proto/telemetry.proto` 从"纸面契约"变成**能跑的真 gRPC 服务**，作为作品集里
> "服务化 / RPC" 的硬证据。**Qt 上位机在 Windows**，gRPC 后端跑在**原生 Ubuntu 22.04（VM / 实体工控机）**。
>
> **环境口径（2026-09-30 起）**：Linux 侧只保留**原生 Ubuntu 22.04** 一套环境。WSL2 已从环境清单**完全退役**
> ——连"构建 / 工具链沙箱"的职责也并入原生 VM（原因见 IEC62304 §1 与《网关与服务链路优化方案》§2）。
> 本指南原先以 WSL2 为落点，现改以原生 Linux 为落点；文末保留一节 WSL 快捷构建，仅作个人本机便利，
> **不再进任何验收/交付口径**。

---

## 0. 为什么用原生 Linux 而不是 WSL2

gRPC 后端是**无头服务**，天然适合 Linux；业务核心库（`frame_protocol.h`/`acquisition.h`）本就是可移植 C++，Linux 直接编。
但 **WSL2 不能进交付/验证口径**：非厂商支持配置、无 `PREEMPT_RT`、服务生命周期随 WSL 会话回收、`/mnt/e` 跨 fs I/O
与 SQLite WAL 锁语义风险、USB 串口须经 `usbipd-win` 打洞。故本项目 Linux 落点统一为**原生 Ubuntu 22.04**
（VM 作验证载体，实体工控机 / 厂商 BSP 作交付形态）。gRPC/protoc 用 apt 一行装（见下），与 Qt 的 MinGW ABI 互不干扰。

---

## 1. 依赖（apt，原生 Ubuntu 22.04）

```bash
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
     libgrpc++-dev protobuf-compiler-grpc protobuf-compiler libprotobuf-dev
```
> apt 版 gRPC **只发布 pkg-config，不发布 `gRPCConfig.cmake`**，故 `service/CMakeLists.txt` 用
> `pkg_check_modules(grpc++)` + `find_program(protoc/grpc_cpp_plugin)` 驱动代码生成，绕开该坑。
> VM 上实测版本：grpc++ 1.30.2 / protobuf 3.12.4（jammy apt 仓库）。

---

## 2. 一键构建 + 端到端自检（推荐）

在**仓库根**跑本项目的验收脚本（它同时是这条链的可复现证据）：
```bash
bash tools/vm_grpc_build_run.sh            # 缺依赖则自动 apt 装 → 构建 → 合成模式端到端冒烟
```
它做四件事并打印 `ASSERT[...]` 与结论行 `PASS=<n> TOTAL=<n>`：
① pkg-config 探测依赖；② `cmake -S service` 构建 `dmp_grpc_server` + `grpc_client_probe`；
③ 后台起**合成模式** server，用 probe 连它跑 `GetStats → GetAlarms → Subscribe(10 条)`；
④（可选）给定 `GATEWAY=host:port` 时让 server 连**已在跑的网关**、验证它是网关的独立 RAW 订阅者。

从 Windows 一条链驱动远端 VM（bundle 同步 → VM 上跑 → 原始读数取回，见 `tools/diagnostics/README.md`）：
```powershell
$env:DMP_VM_IP='<VM 地址>'; $env:DMP_VM_USER='dirk'
pwsh -NoProfile -File tools/diagnostics/run_vm_rounds.ps1 -RepoScript tools/vm_grpc_build_run.sh
```

期望读数（数值随合成数据变化）：
```
[STATS]  ok=... crc_err=0 dropped=0
[ALARMS]  - HR out of range (HR=...)
[SUBSCRIBE] 接收 10 条实时样本:  SAMPLE ts=... seq=... ch=.. type=.. value=..
probe done.
```
最近一次实跑证据：`docs/grpc-verify-log.md` §6（原生 Ubuntu 22.04.5 / HEAD 42b757c：`ASSERT[synthetic]=PASS` + `ASSERT[raw_upstream]=PASS`（自起 device_simulator，ok=48 samples=5 crc_err=0）、`PASS=2 TOTAL=2`）。

> 关键设计：合成模式自造 ~20Hz 四通道数据喂进真实 `Acquisition` 解码/告警链，**不依赖任何外部进程**，开箱即绿。

---

## 3. 手动分步（想看清每步时）

```bash
cmake -S service -B build_linux_grpc -DCMAKE_BUILD_TYPE=Release
cmake --build build_linux_grpc -j

# 终端A: 合成模式 server
./build_linux_grpc/dmp_grpc_server 50051
# 终端B: probe
./build_linux_grpc/grpc_client_probe 127.0.0.1:50051 10
```
代码生成由 `service/CMakeLists.txt` 里的 `protoc` / `grpc_cpp_plugin` 自动完成，无需手动跑 protoc。

---

## 4.（进阶）让 gRPC server 吃真实帧流

`grpc_server` 上游模式：`./dmp_grpc_server <port> <host> <upstream_port>` —— 连一个 TCP 帧流源，
**先送一行 `RAW\n` 再按字节收帧**（对 `device_simulator` 无害，对 `gateway_service` 开启 RAW 逐字节透传）。
因此 `grpc_server` 与 ROS2 桥一样，是网关的**独立 RAW 订阅者**；一台 Linux 主机上两者可与 `gw_probe` 并存。

---

## 5. 与文本网关的关系（别混淆）

| 服务层实现 | 传输 | 跑在哪 | 用途 |
|---|---|---|---|
| `src/gateway_service.cpp` | TCP 文本行 (HELP/STATS/…) | Windows（Linux 形态见优化方案） | 零依赖、可演示、`gw_probe.ps1` 验证 |
| `service/grpc_server.cpp` | **gRPC/HTTP2 protobuf** | **原生 Ubuntu 22.04（VM/工控机）** | 真 gRPC 契约落地，`vm_grpc_build_run.sh` / `grpc_client_probe` 验证 |

两者**同语义、共用同一 `Acquisition` 解码/告警内核**，演示"业务层不动、只换传输层"的服务化能力。

---

## 附 A：常见问题
- **`Could not find gRPCConfig.cmake`**：apt 版 gRPC 不发布 CMake config，`service/CMakeLists.txt` 已改用 pkg-config；
  若仍失败先 `pkg-config --exists grpc++ && echo ok` 确认 `libgrpc++-dev` 已装。
- **`grpc_cpp_plugin` 找不到**：`protobuf-compiler-grpc` 提供，`which grpc_cpp_plugin` 应在 `/usr/bin`。
- **链接期报缺 `gpr`/`grpc`/`absl_*`**：`grpc++.pc` 已含传递依赖；手工链时确保 `${GRPCPP_LIBRARIES}` 完整带入。
- **probe 连不上**：先确认 server 日志 `Telemetry server listening on 0.0.0.0:50051`；同机 `127.0.0.1` 即可。

## 附 B：个人本机快捷（WSL2，已退役出环境清单）
> 仅供手头没 VM 时快编一版；**不作为交付/验证口径**（理由见 §0）。工程在 WSL 路径 `/mnt/e/...`，
> 用 `bash wsl_setup.sh`（构建目录 `build_wsl`）。`/mnt/e` 是 DrvFs，编译较慢；要快可 `cp` 到 `~/dmp` 再构建。
> 若 WSL 默认 NAT 下要连 Windows 侧模拟器，需开镜像网络（`.wslconfig` 设 `networkingMode=mirrored`）或用默认路由网关 IP。
