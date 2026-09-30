# gRPC 服务层验证日志（可复现证据）

> 本文记录 `service/grpc_server.cpp` 真 gRPC 服务的一次完整"构建 + 端到端自检"通过实况，作为作品集的**可验证证据**。所有输出为真实运行结果（非模拟）。
> §1~§5 那次跑在 **WSL2**（2026-09-28，历史记录）；WSL2 退役后，§6 是迁至**原生 Ubuntu 22.04 VM** 的复跑实况（2026-09-30）——**口径以 §6 为准**。

- **日期**：2026-09-28
- **主机**：Windows，经 `wsl.exe` 进入
- **复现命令**：`cd /mnt/e/Work/McuProject/DeviceMonitorPlatform && bash wsl_setup.sh --build`

---

## 1. 环境探测

```
NAME            STATE           VERSION
* Ubuntu-22.04    Running         2          # WSL2

os:      Ubuntu 22.04.5 LTS
arch:    x86_64
protoc:  libprotoc 3.12.4
grpc_cpp_plugin: /usr/bin/grpc_cpp_plugin
pkg-config grpc++: 1.30.2
cmake:   cmake version 3.22.1
```

**关键坑与解法**：apt 版 gRPC **不发布 `gRPCConfig.cmake`**（只发布 pkg-config），故 `find_package(gRPC CONFIG)` 必失败。`service/CMakeLists.txt` 改用 `pkg_check_modules(grpc++)` + `find_program(protoc/grpc_cpp_plugin)` 驱动代码生成，绕开该问题。

---

## 2. 构建日志（截取）

```
-- Checking for module 'grpc++'
--   Found grpc++, version 1.30.2
-- Checking for module 'protobuf'
--   Found protobuf, version 3.12.4
-- Configuring done
-- Generating done

[ 22%] protoc: 生成 protobuf C++
[ 22%] protoc: 生成 gRPC C++ 桩
[ 44%] Building CXX object CMakeFiles/telemetry_gen.dir/gen/telemetry.pb.cc.o
[ 44%] Building CXX object CMakeFiles/telemetry_gen.dir/gen/telemetry.grpc.pb.cc.o
[ 55%] Linking CXX static library libtelemetry_gen.a
[ 55%] Built target telemetry_gen
[ 77%] Building CXX object CMakeFiles/dmp_grpc_server.dir/grpc_server.cpp.o
[ 77%] Building CXX object CMakeFiles/grpc_client_probe.dir/grpc_client_probe.cpp.o
[ 88%] Linking CXX executable grpc_client_probe
[100%] Linking CXX executable dmp_grpc_server
[100%] Built target grpc_client_probe
[100%] Built target dmp_grpc_server
```

首轮构建曾暴露 2 个真·编译错误（均已修，非环境问题）：
1. `rng(0xDMP)` —— `P`/`M` 非十六进制数字，非法字面量 → 改整数种子。
2. `Subscribe` 里 `std::vector<Sample_Type> req->types().begin(), ...` —— proto 生成的 `repeated Type` 实为 `RepeatedField<int>`，不能用 `int*` 迭代器构造枚举 vector（触发 `static_assert`）→ 改为按 `int` 收集再比较。

---

## 3. 端到端自检（合成模式：自造 ~20Hz 四通道数据）

```
[4/4] 冒烟自检: 后台起合成模式 server, 用 probe 连它跑 GetStats/GetAlarms/Subscribe ...
[STATS]  ok=120 crc_err=0 dropped=0
[ALARMS]
  - HR out of range  (HR=116.656 critical=1)
[SUBSCRIBE] 接收 10 条实时样本:
  SAMPLE ts=1790565390809 seq=120 ch=0 type=TEMP value=36.5297
  SAMPLE ts=1790565390809 seq=121 ch=1 type=HR   value=72.5699
  SAMPLE ts=1790565390809 seq=122 ch=2 type=SPO2 value=95.1499
  SAMPLE ts=1790565390809 seq=123 ch=3 type=CONC value=4.69873
  SAMPLE ts=1790565390859 seq=124 ch=0 type=TEMP value=36.5607
  SAMPLE ts=1790565390859 seq=125 ch=1 type=HR   value=74.2161
  SAMPLE ts=1790565390909 seq=126 ch=2 type=SPO2 value=95.6036
  SAMPLE ts=1790565390909 seq=127 ch=3 type=CONC value=4.64443
  SAMPLE ts=1790565390909 seq=128 ch=0 type=TEMP value=36.6616
  SAMPLE ts=1790565390909 seq=129 ch=1 type=HR   value=71.0817
[SUBSCRIBE] 结束 (cancelled)
probe done.
自检完成。详见 /tmp/dmp_grpc.log
```

---

## 4. 结果解读

| rpc | 类型 | 结果 | 说明 |
|---|---|---|---|
| `GetStats` | 一元 | `ok=120 crc_err=0 dropped=0` | 120 帧全部解码成功，CRC 零误报，无丢帧 |
| `GetAlarms` | 服务端流 | `HR out of range (HR=116.656 critical=1)` | 心率越界(>110)命中阈值告警，`critical` 标志正确 |
| `Subscribe` | 实时流 | 接收 10 条样本，四通道值/类型/seq 连续 | 订阅后按游标追读、客户端 `TryCancel` 正常收尾 |

**跨端一致性**：解码/告警内核在 Linux 侧与 Windows 完全同源（`crc_err=0` 即证帧协议两端字节级一致）。

---

## 5. 服务层三种同语义实现（本项目的"契约与实现解耦"实证）

1. **Windows 文本行 TCP 网关** —— `src/gateway_service.cpp`（实测 `ok=52 crc_err=0`）
2. **gRPC 契约** —— `proto/telemetry.proto`
3. **WSL2 真 gRPC 服务** —— `service/grpc_server.cpp`（本文，实测 `ok=120` + 流式订阅 + 告警）

三者共用同一 `dmp::Acquisition` 解码/告警内核，业务层不动、只换传输层——可现场演示。

---

## 6. 迁移到原生 Linux / VM 复跑（2026-09-30，WSL2 退役后）

WSL2 从环境清单完全退役后，gRPC 服务层迁入**原生 Ubuntu 22.04 VM**并实跑做实。经 `tools/diagnostics/run_vm_rounds.ps1`（git bundle 同步 → VM 上 `bash tools/vm_grpc_build_run.sh` → 原始读数 scp 取回）：

- **主机**：VM，`Ubuntu 22.04.5 LTS` / Linux `6.8.0-138-generic` / HEAD `4a2acde`
- **依赖**：apt `grpc++ 1.30.2`、`protobuf 3.12.4`、`protoc` + `/usr/bin/grpc_cpp_plugin`
- **构建**：`cmake -S service` 产出 `dmp_grpc_server` + `grpc_client_probe` → `ASSERT[build]=PASS`
- **合成模式端到端**：`ASSERT[synthetic]=PASS (probe_rc=0 samples=10 crc_err=0)` —— `GetStats`/`GetAlarms`/`Subscribe(10 条)` 全通过
- **RAW 上游订阅**：`ASSERT[raw_upstream]=PASS` —— 两种拓扑均跑通：① 直连 `device_simulator`（HEAD `42b757c`，`ok=48 samples=5 crc_err=0`）；② **经完整链路 `device_simulator:9000 → gateway_service:9100 → grpc_server`**（HEAD `4a2acde`，`ok=44 samples=5 crc_err=0`）—— 后者才是“grpc_server 作为**网关**的独立 RAW 订阅者”的原生 VM 版（发 `RAW\n` 逐字节收帧解码，正流断言非 0-对-0）
- **结论行**：`PASS=2 TOTAL=2`（最新 HEAD `4a2acde`）

> 判据由脚本本轮现算，非写死；`crc_err=0` 再次佐证帧协议内核在 Linux 侧与 Windows 字节级同源。
