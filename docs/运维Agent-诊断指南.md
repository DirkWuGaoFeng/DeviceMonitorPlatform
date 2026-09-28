# 运维诊断 Agent 指南（`tools/dmp_agent.py`）

> 一句话：在遥测网关之上挂一个"值班工程师 Agent"，用自然语言问 **"3 号通道为何频繁告警？"**，它调用真实网关数据给出**有数字证据的归因结论**。纯 Python 标准库，零第三方依赖。

## 它在架构里的位置

```
STM32(USART1) → gateway_service(:9100, 文本行协议) ←── tools/dmp_agent.py（运维 Agent）
                        ↑                                   │ STATS/ALARMS/HISTORY/SUBSCRIBE
                     grpc_server(WSL) ← 同一网关 RAW 透传    └ 归因报告打印到终端
```

Agent 是网关文本协议（`dmp/service_proto.h`）的**又一个消费者**，与 Qt 上位机、gRPC 服务并列——印证"传输/业务解耦，客户端可插拔"。

## 为 Agent 新增的能力：`HISTORY` 命令

告警归因需要"这段时间某通道到底什么样"，光有实时 `SUBSCRIBE` 不够。故给协议加了窗口聚合命令：

```
HISTORY [ch] [windowSec]   默认全部通道 / 60 秒
→ HISTORY ch=1 count=147 avg=74.8 min=50.0 max=129.6 last=71.2   (一行一通道)
```

- 网关侧维护一个有界样本环（30000 条 ≈ 150KB），`HISTORY` 按 `nowMs - windowSec` 过滤后聚合 count/avg/min/max/last。
- 已进协议层单测：`test_service_proto.cpp` 覆盖 `HISTORY` 解析（缺省/参数/非法回退）与 `formatHistory` 逐字节输出，**25 断言全过**。

## 两种模式

### 1. 离线确定性诊断（默认，无需密钥，CI 可跑）
固定跑 4 个工具调用（`get_stats` → `get_alarms` → `channel_history` → 订阅 N 秒实时采样），再用规则推理生成报告：

```powershell
# 先确保网关在跑: .\run_gateway_serial.ps1
python tools\dmp_agent.py "心率通道为何频繁告警？" --collect 14
```

真机实测输出（固件注入周期性心率尖峰）：
```
[诊断报告]
- 告警共 9 条, 最集中: CH1(心率) 占 100%, 触发规则: HR out of range
- CH1 120s 画像: avg=74.8, 波动 50.0~129.6 (极差 79.6)
- 阈值带 [50.0, 110.0]; 实时 17 点中越上限 1 点
- 归因: 基线均值(74.8)正常, 但峰值冲高至 129.6 远超上限 110.0 ——
        这是'正常波形上叠加周期性越限尖峰'的特征(固件演示注入), 而非生理异常或链路故障。
  佐证: crc_err=0/dropped=0 传输面健康; 越限为离散点非连续段, 与周期注入一致。
- 传输面: ok=4613 crc_err=0 dropped=0 -> 无劣化
```

归因规则会区分四种形态：**周期尖峰**（基线正常+峰值远超上限）、**持续偏高/偏低**（均值越带）、**随机分布**（无明显特征）——每种给出不同处置建议。

### 2. LLM function-calling（`--llm`，接任意 OpenAI 兼容端点）
把三个工具以标准 `tools` schema 交给大模型，模型自主决定调用链，最多 6 轮后产出自然语言结论。端点/模型/密钥全走环境变量，可指向你自建 Agent 平台或任意兼容 API：

```powershell
$env:DMP_LLM_BASE_URL='https://<你的兼容端点>/v1'
$env:DMP_LLM_KEY='sk-...'
$env:DMP_LLM_MODEL='<模型名>'
python tools\dmp_agent.py "最近病人体征稳定吗？" --llm
```
未配置端点时自动回退离线模式，不会崩。

## 顺带揪出的固件潜伏 bug（面试好素材）

调 Agent 归因时发现"心率每 40 拍注入 +30 危急值"始终不触发告警。追到固件 `dmp_frame.c`：
- 原逻辑 `if (g_step % 40 == 0) hr += 30;` 放在 `case 1:`（心率）分支里；
- 但 `g_step` 与轮转通道 `g_ch` **同速递增**，`g_step % 40 == 0` 成立时 `g_ch` 恒为 `0`（体温分支）——**心率尖峰永远发不出**。
- 修法：给心率分支一个**专属静态计数** `g_hr_n`，每 15 个心率样本（≈12s）必注入一次 `125±5`，与相位无关、确定可复现。

> 这正好演示了"上位机数据分析 → 反向暴露下位机逻辑缺陷"的闭环能力：不是拍脑袋改固件，而是 Agent 报告里"心率 max 只有 91、从不越限"这一异常画像，把固件的相位锁死 bug 逼了出来。

## 参数速查
| 参数 | 默认 | 说明 |
|---|---|---|
| `question` | "3号通道为何频繁告警?" | 自然语言问题（离线模式下用于报告抬头） |
| `--host/--port` | 127.0.0.1 / 9100 | 网关地址 |
| `--collect` | 8 | 实时 SUBSCRIBE 采样秒数 |
| `--llm` | 关 | 启用大模型 function-calling |
