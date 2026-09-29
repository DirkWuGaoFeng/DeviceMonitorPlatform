# WSL2 落地真 gRPC 服务层 —— 操作指南

> 目标：把 `proto/telemetry.proto` 从"纸面契约"变成**能跑的真 gRPC 服务**，作为求职作品集里
> "服务化 / RPC" 的硬证据。**Qt 上位机仍在 Windows**，只有 gRPC 后端放 WSL —— 二者解耦，各司其职。

---

## 0. 为什么选 WSL 而不是 Windows 原生

| 方案 | 装 gRPC 难度 | 与 MinGW Qt 的 ABI |
|---|:--:|---|
| Windows `vcpkg install grpc` | 🔴 源码编译数十分钟，默认 MSVC | 与 Qt 的 MinGW **ABI 冲突** |
| **WSL2 apt** | 🟢 `apt install libgrpc++-dev protobuf-compiler-grpc` 一行 | 独立 Linux 进程，**无冲突** |

gRPC 后端是**无头服务**，天然适合放 Linux；业务核心库(`frame_protocol.h`/`acquisition.h`)本就是可移植 C++，Linux 直接编。

---

## 1. 一次性准备：启用 WSL2 + Ubuntu

在 **Windows PowerShell（管理员）**：
```powershell
wsl --install -d Ubuntu-24.04      # 已装则: wsl -l -v 查看
```
装完进 Ubuntu 设个用户名/密码。你的工程在 WSL 里的路径是：
```
/mnt/e/Work/McuProject/DeviceMonitorPlatform
```

---

## 2. 一键构建 + 自检（推荐先跑这个）

在 **WSL2 终端**：
```bash
cd /mnt/e/Work/McuProject/DeviceMonitorPlatform
bash wsl_setup.sh
```
它会：① `apt` 装 gRPC/protobuf/cmake；② CMake 配置生成 `build_wsl/`；③ 编译
`dmp_grpc_server` + `grpc_client_probe`；④ **合成模式冒烟**：后台起 server，用 probe 连它跑
`GetStats → GetAlarms → Subscribe(10 条)`，打印结果。

期望看到（数值会随合成数据变化）：
```
[STATS]  ok=... crc_err=0 dropped=0
[ALARMS]  - HR out of range (HR=...)   # 心率偶发越界
[SUBSCRIBE] 接收 10 条实时样本:
  SAMPLE ts=... seq=... ch=0 type=TEMP value=36.8
  ...
probe done.
```

> 关键设计：server 默认**合成模式**——自己按 ~20Hz 造 4 通道数据喂进真实 `Acquisition` 解码/告警链，
> **不依赖任何 Windows 进程、不碰跨主机网络**，所以开箱即绿。

---

## 3. 手动分步（想看清每步时）

```bash
# 依赖
sudo apt-get update
sudo apt-get install -y build-essential cmake pkg-config \
     libgrpc++-dev protobuf-compiler-grpc protobuf-compiler libprotobuf-dev

# 构建
cmake -S service -B build_wsl -DCMAKE_BUILD_TYPE=Release
cmake --build build_wsl -j

# 终端A: 起 server(合成模式)
./build_wsl/dmp_grpc_server 50051

# 终端B: 起 probe
./build_wsl/grpc_client_probe 127.0.0.1:50051 10
```

代码生成由 `service/CMakeLists.txt` 里的 `protoc`/`grpc_cpp_plugin` 自动完成
（`--cpp_out` 生成 `telemetry.pb.*`，`--grpc_out` 生成 `telemetry.grpc.pb.*`），无需手动跑 protoc。

---

## 4.（进阶）让 gRPC server 吃 Windows 侧模拟器的真实帧流

合成模式够演示；若想让 Windows 上已构建的 `build\device_simulator.exe`(:9000) 当真上游：

**坑**：WSL2 默认 **NAT 网络模式**下，WSL 里的 `127.0.0.1` **到不了** Windows 的监听端口。两种解法：

- **解法 A（推荐，Windows 11 22H2+）**：开**镜像网络**，让两边 `localhost` 打通。
  `%UserProfile%\.wslconfig` 加：
  ```ini
  [wsl2]
  networkingMode=mirrored
  ```
  保存后 `wsl --shutdown` 重进 WSL。然后：
  ```bash
  ./build_wsl/dmp_grpc_server 50051 127.0.0.1 9000     # 连 Windows 上的模拟器
  ```
  （Windows 侧先 `.\build\device_simulator.exe 9000`，注意防火墙放行。）

- **解法 B（不改配置）**：用 Windows 主机在 WSL 里的网关 IP（NAT 模式下即默认路由）：
  ```bash
  WINIP=$(ip route show default | awk '{print $3}')
  ./build_wsl/dmp_grpc_server 50051 "$WINIP" 9000
  ```

---

## 5. 与文本网关的关系（别混淆）

| 服务层实现 | 传输 | 跑在哪 | 用途 |
|---|---|---|---|
| `src/gateway_service.cpp` | TCP 文本行 (HELP/STATS/…) | **Windows** | 零依赖、可演示、`gw_probe.ps1` 验证 |
| `service/grpc_server.cpp` | **gRPC/HTTP2 protobuf** | **WSL2** | 真 gRPC 契约落地，`grpc_client_probe` 验证 |

两者**同语义、共用同一 `Acquisition` 解码/告警内核**，正好演示“业务层不动、只换传输层”的服务化能力。

---

## 附：常见问题
- **`Could not find ... gRPCConfig.cmake`（旧版 CMakeLists 会报）**：apt 版 gRPC **不发布 CMake config 文件**，只发布 pkg-config。当前 `service/CMakeLists.txt` 已改用 `pkg_check_modules(grpc++)` + `find_program(protoc/grpc_cpp_plugin)`，直接 `bash wsl_setup.sh` 即可；若仍失败先跑 `pkg-config --exists grpc++ && echo ok` 确认 `libgrpc++-dev` 已装。
- **`grpc_cpp_plugin` 找不到**：`protobuf-compiler-grpc` 提供，`which grpc_cpp_plugin` 应在 `/usr/bin`。
- **Protobuf 3.12.4**：说明你在 **Ubuntu 20.04(focal)**，其 gRPC 为 1.26（尚无 absl 依赖），pkg-config 方案天然适配。
- **链接期报缺 `gpr`/`grpc`/`absl_*`**：`grpc++.pc` 已含传递依赖；若手工链，确保 `${GRPCPP_LIBRARIES}` 被完整带入（本 CMake 已 `target_link_libraries`）。
- **/mnt/e 编译很慢**：正常（DrvFs 跨文件系统 I/O 开销）。要快可把工程 `cp` 到 `~/dmp` 再构建。
- **probe 连不上**：先确认 server 日志 `listening on 0.0.0.0:50051`；同机 `127.0.0.1` 即可，无需跨主机。
