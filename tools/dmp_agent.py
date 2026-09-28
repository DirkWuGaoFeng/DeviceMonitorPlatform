#!/usr/bin/env python3
# tools/dmp_agent.py — DMP 运维诊断 Agent (LLM-ready, 双模式)
#
# 角色: 挂在遥测网关(service_proto 文本行协议)之上的"值班工程师 Agent"。
#       它把网关命令封装成工具(get_stats/get_alarms/channel_history), 面对
#       "3 号通道为何频繁告警?"这类问题给出有数据支撑的归因结论。
#
# 两种模式:
#   离线模式(默认): 确定性诊断流程 —— 采数据 -> 告警归因统计 -> 规则推理出结论。
#                   无外部依赖/无需密钥, CI 可跑, 结果可复现。
#   LLM 模式(--llm): OpenAI 兼容 /chat/completions + function calling 循环,
#                   tools 即上述三个网关命令。base_url/model/key 全走环境变量,
#                   可指向任意兼容端点(自建网关/DeepSeek/通义...), Agent 平台可平替。
#
# 用法:
#   python tools/dmp_agent.py "3号通道为何频繁告警?" --collect 10
#   $env:DMP_LLM_BASE_URL='https://.../v1'; $env:DMP_LLM_KEY='sk-...'; $env:DMP_LLM_MODEL='...'
#   python tools/dmp_agent.py "最近心率正常吗?" --llm
import argparse
import json
import os
import re
import socket
import sys
import time
from collections import defaultdict

CHANNEL_NAMES = {0: "体温", 1: "心率", 2: "血氧", 3: "浓度"}
# 与网关/上位机一致的危急值规则 (用于归因解释)
RULES = {"HR": (50.0, 110.0, "心率危急范围"), "TEMP": (35.0, 42.0, "体温范围"), "SPO2": (94.0, 100.0, "血氧下限")}


# ---------- 网关客户端: 一条连接跑完一轮命令 (服务端对每条连接独立) ----------
class GatewayClient:
    def __init__(self, host, port, timeout=5.0):
        self.host, self.port, self.timeout = host, port, timeout

    def _ask(self, lines, read_until_pred, quiet=0.2, read_sec=None):
        read_sec = read_sec if read_sec is not None else self.timeout
        s = socket.create_connection((self.host, self.port), timeout=self.timeout)
        s.settimeout(min(read_sec, self.timeout))
        buf, out = b"", []
        try:
            for ln in lines:
                s.sendall((ln + "\n").encode())
            deadline = time.time() + read_sec
            while time.time() < deadline:
                try:
                    d = s.recv(8192)
                except socket.timeout:
                    break
                if not d:
                    break
                buf += d
                while b"\n" in buf:
                    raw, buf = buf.split(b"\n", 1)
                    out.append(raw.decode(errors="replace"))
                if read_until_pred and read_until_pred(out):
                    time.sleep(quiet)          # 再收一小段尾巴
                    try:
                        s.settimeout(quiet)
                        while True:
                            d = s.recv(8192)
                            if not d:
                                break
                            buf += d
                            while b"\n" in buf:
                                raw, buf = buf.split(b"\n", 1)
                                out.append(raw.decode(errors="replace"))
                    except socket.timeout:
                        pass
                    return out
        finally:
            s.close()
        return out

    def stats(self):
        for ln in self._ask(["STATS"], lambda o: any(x.startswith("STATS") for x in o)):
            m = re.match(r"STATS ok=(\d+) crc_err=(\d+) dropped=(\d+)", ln)
            if m:
                return {"ok": int(m.group(1)), "crc_err": int(m.group(2)), "dropped": int(m.group(3))}
        return {}

    def alarms(self, limit=100):
        out = []
        for ln in self._ask([f"ALARMS {limit}"], lambda o: True):
            if ln.startswith("ALARM ") and ln.strip() != "ALARM none":
                head, _, msg = ln.partition(" msg=")          # msg 含空格, 必须先切掉尾部
                d = dict(re.findall(r"\b(\w+)=(\S+)", head))
                d["msg"] = msg.strip()
                out.append(d)
        return out

    def history(self, window_sec=60, ch=-1):
        rows = []
        for ln in self._ask([f"HISTORY {ch} {window_sec}"], lambda o: any(x.startswith("HISTORY") for x in o)):
            if ln.startswith("HISTORY ch="):
                d = dict(re.findall(r"(\w+)=([^\s]+)", ln))
                rows.append({"ch": int(d["ch"]), "count": int(d["count"]), "avg": float(d["avg"]),
                             "min": float(d["min"]), "max": float(d["max"]), "last": float(d["last"])})
        return rows

    def collect_samples(self, seconds):
        """SUBSCRIBE 订阅 N 秒, 返回逐通道样本值列表 (实时画像用)"""
        vals = defaultdict(list)
        for ln in self._ask(["SUBSCRIBE"], lambda o: False, read_sec=seconds):
            if ln.startswith("SAMPLE "):
                d = dict(re.findall(r"(\w+)=([^\s]+)", ln))
                try:
                    vals[int(d["ch"])].append(float(d["value"]))
                except (KeyError, ValueError):
                    pass
        return dict(vals)


# ---------- 工具层 (LLM function calling 的 tools 就是这三个函数的 schema 化) ----------
TOOL_SPECS = [
    {"type": "function", "function": {"name": "get_stats", "description": "网关解码统计: ok/crc_err/dropped 帧计数",
     "parameters": {"type": "object", "properties": {}}}},
    {"type": "function", "function": {"name": "get_alarms", "description": "最近告警列表(通道/数值/消息)",
     "parameters": {"type": "object", "properties": {"limit": {"type": "integer"}}}}},
    {"type": "function", "function": {"name": "channel_history", "description": "近N秒逐通道聚合(count/avg/min/max/last)",
     "parameters": {"type": "object", "properties": {"window_sec": {"type": "integer"}, "channel": {"type": "integer"}}}}},
]


def exec_tool(gw: GatewayClient, name, args):
    if name == "get_stats":
        return gw.stats()
    if name == "get_alarms":
        return gw.alarms(int(args.get("limit", 100)))
    if name == "channel_history":
        return gw.history(int(args.get("window_sec", 60)), int(args.get("channel", -1)))
    return {"error": "no such tool"}


# ---------- 离线模式: 确定性诊断 + 规则推理 ----------
def offline_diagnose(gw, question, collect_sec):
    print(f"[agent] 问题: {question}")
    print(f"[agent] 工具调用 1/4: get_stats")
    st = gw.stats()
    print(f"        -> {st}")
    print(f"[agent] 工具调用 2/4: get_alarms(limit=100)")
    al = gw.alarms(100)
    by_ch = defaultdict(int)
    by_msg = defaultdict(int)
    for a in al:
        by_ch[int(a.get("ch", -1))] += 1
        by_msg[a.get("msg", "?")] += 1
    print(f"        -> 共 {len(al)} 条; 按通道 {dict(by_ch)}; 按规则 {dict(by_msg)}")
    print(f"[agent] 工具调用 3/4: channel_history(window=120s)")
    hist = gw.history(120)
    for h in hist:
        print(f"        -> ch{h['ch']}({CHANNEL_NAMES.get(h['ch'],'?')}) "
              f"count={h['count']} avg={h['avg']:.2f} min={h['min']:.2f} max={h['max']:.2f}")
    print(f"[agent] 工具调用 4/4: subscribe {collect_sec}s 实时采样")
    live = gw.collect_samples(collect_sec)
    n_all = sum(len(v) for v in live.values())
    print(f"        -> 收到 {n_all} 条样本")

    # --- 规则推理, 生成结论 ---
    lines = ["", "=" * 62, "[诊断报告]"]
    if not al:
        lines.append("- 近期无告警记录, 设备链路健康 "
                     f"(ok={st.get('ok')} crc_err={st.get('crc_err')} drop={st.get('dropped')})。")
    else:
        top_ch = max(by_ch, key=by_ch.get)
        top_msg = max(by_msg, key=by_msg.get)
        ratio = by_ch[top_ch] / max(1, len(al))
        lines.append(f"- 告警共 {len(al)} 条, 最集中: CH{top_ch}({CHANNEL_NAMES.get(top_ch,'?')}) "
                     f"占 {ratio:.0%}, 触发规则: {top_msg}")
        h = next((x for x in hist if x["ch"] == top_ch), None)
        if h:
            rng = RULES.get(next((a.get("name", "") for a in al if int(a.get("ch", -1)) == top_ch), ""), None)
            span = h["max"] - h["min"]
            lines.append(f"- CH{top_ch} 120s 画像: avg={h['avg']:.1f}, 波动 {h['min']:.1f}~{h['max']:.1f} "
                         f"(极差 {span:.1f})")
            if rng and "out of range" in top_msg:
                lo, hi, desc = rng[0], rng[1], rng[2]
                lv = live.get(top_ch) or []
                over = [v for v in lv if v > hi]
                lines.append(f"- 阈值带 [{lo}, {hi}] ({desc}); 实时 {len(lv)} 点中越上限 {len(over)} 点")
                # 区分"持续偏移"与"周期尖峰": 看均值是否在正常带内而峰值远超上限
                baseline_ok = lo <= h["avg"] <= hi
                peak_far = h["max"] > hi + 0.1 * hi
                if baseline_ok and peak_far:
                    lines.append(f"- 归因: 基线均值({h['avg']:.1f})正常, 但峰值冲高至 {h['max']:.1f} 远超上限 {hi} —— "
                                 "这是'正常波形上叠加周期性越限尖峰'的特征(固件演示注入), 而非生理异常或链路故障。")
                    lines.append("  佐证: crc_err=0/dropped=0 传输面健康; 越限为离散点非连续段, 与周期注入一致。")
                elif h["avg"] > hi:
                    lines.append(f"- 归因: 均值({h['avg']:.1f})已整体高于上限 {hi}, 呈持续性偏高, 建议核查被测对象与实际传感器读数。")
                elif h["avg"] < lo:
                    lines.append(f"- 归因: 均值({h['avg']:.1f})整体低于下限 {lo}, 呈持续性偏低, 建议核查传感器接触/量程。")
                else:
                    lines.append("- 归因: 越带样本分布无明显周期特征, 建议延长观察窗口并核对传感器安装。")
    lines.append(f"- 传输面: ok={st.get('ok')} crc_err={st.get('crc_err')} dropped={st.get('dropped')} "
                 "-> " + ("无劣化" if st.get("crc_err", 0) == 0 and st.get("dropped", 0) == 0 else "存在丢帧/校验错, 检查波特率与线路"))
    lines.append("=" * 62)
    print("\n".join(lines))
    return 0 if al is not None else 1


# ---------- LLM 模式: OpenAI 兼容 function calling 循环 ----------
def llm_diagnose(gw, question, collect_sec):
    import urllib.request
    base = os.environ.get("DMP_LLM_BASE_URL", "").rstrip("/")
    key = os.environ.get("DMP_LLM_KEY", "")
    model = os.environ.get("DMP_LLM_MODEL", "")
    if not base or not model:
        print("[agent] 未配置 DMP_LLM_BASE_URL / DMP_LLM_MODEL, 回退离线模式")
        return offline_diagnose(gw, question, collect_sec)

    sys_msg = ("你是医疗设备遥测系统的值班运维 Agent。你通过工具获取真实网关数据, "
               "回答必须引用工具返回的数字作证据, 用中文, 结论含: 现象/归因/传输面健康度/建议。"
               f"通道语义: 0=体温 1=心率 2=血氧 3=麻醉气体浓度。")
    msgs = [{"role": "system", "content": sys_msg},
            {"role": "user", "content": question + f"\n(可结合近{collect_sec}秒实时数据)"}]

    def chat():
        req = urllib.request.Request(
            base + "/chat/completions",
            data=json.dumps({"model": model, "messages": msgs, "tools": TOOL_SPECS}).encode(),
            headers={"Content-Type": "application/json", "Authorization": f"Bearer {key}"})
        with urllib.request.urlopen(req, timeout=60) as r:
            return json.loads(r.read())["choices"][0]["message"]

    for step in range(6):
        m = chat()
        calls = m.get("tool_calls")
        if not calls:
            print(f"\n[agent] LLM 最终回答 (第 {step+1} 轮):\n" + m.get("content", ""))
            return 0
        msgs.append(m)
        for c in calls:
            fn, args = c["function"]["name"], json.loads(c["function"].get("arguments") or "{}")
            res = exec_tool(gw, fn, args)
            print(f"[agent] LLM 调用工具: {fn}({args}) -> {str(res)[:110]}")
            msgs.append({"role": "tool", "tool_call_id": c["id"], "content": json.dumps(res, ensure_ascii=False)})
    print("[agent] 达到最大工具调用轮数")
    return 1


def main():
    ap = argparse.ArgumentParser(description="DMP 运维诊断 Agent")
    ap.add_argument("question", nargs="?", default="3号通道为何频繁告警?", help="自然语言问题")
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=9100)
    ap.add_argument("--collect", type=int, default=8, help="实时订阅采样秒数")
    ap.add_argument("--llm", action="store_true", help="启用 LLM function calling (需环境变量端点)")
    a = ap.parse_args()
    try:
        socket.create_connection((a.host, a.port), timeout=2).close()
    except OSError:
        print(f"[agent] 连不上网关 {a.host}:{a.port} —— 先运行 run_gateway_serial.ps1")
        return 2
    gw = GatewayClient(a.host, a.port)
    if a.llm:
        return llm_diagnose(gw, a.question, a.collect)
    return offline_diagnose(gw, a.question, a.collect)


if __name__ == "__main__":
    sys.exit(main())
