#!/usr/bin/env python3
"""slow_client_probe.py — 慢客户端注入实测（对应 SR-014 / 消减 R-009 的验收判据）

要证明的因果（不是"看起来没崩"）：
  一个连着 SUBSCRIBE 却**一个字节都不读**的客户端，不得拖慢其他订阅者，也不得让上游停摆；
  它的代价必须被计在**它自己**的出向队列上，并且三档丢弃（挤掉 evict / 超长拒收 / 踢除 kick）
  能被分开归因；同时它自己的 seq 缺口要能和网关报的丢弃数**对得上账**。

用法（两端同一支脚本）：
  python tools/diagnostics/slow_client_probe.py --gw build_s1/gateway_service --sim build_s1/device_simulator
  python tools/diagnostics/slow_client_probe.py --gw build_tmp/gateway_service.beforefix --sim build_s1/device_simulator --window 50
                                                     ^ 修复前的二进制（阻塞式 sendAll，无应用层上限）：
                                                       本脚本必须在这臂上报 FAIL，否则“全绿”只说明
                                                       探针测不到这件事。两臂用**同一个**窗口长度（脚本默认 50s）：
                                                       上轮给两臂配不同窗口是错的——堆满内核发送缓冲是对照臂
                                                       自己的事，不该靠改窗口去补偿被测方。剂量算术见 --window/--flood-cap 旁注释；
                                                       对照臂没有 CLIENTS 命令，只判得到不依赖它的 6 格（J1/J2/J3/J4/J8/J10）。
"""
import argparse
import os
import re
import signal
import socket
import subprocess
import sys
import threading
import time

IS_WIN = os.name == "nt"
EXE = ".exe" if IS_WIN else ""
P_SIM, P_GW = (49300, 49301) if IS_WIN else (59300, 59301)
SEQ_MOD = 1 << 16

SAMPLE_RE = re.compile(r"SAMPLE ts=\d+ seq=(\d+) ")
CLIENT_RE = re.compile(
    r"CLIENT id=(\d+) sub=(\d+) raw=(\d+) qbytes=(\d+) lines=(\d+) stalled=(\d+) "
    r"enq=(\d+) sent=(\d+) drop=(\d+) evict=(\d+) kick=(\d+)")


class Reader(threading.Thread):
    """一路订阅者：持续读，统计行数与 seq 缺口（缺口按"跳过了几个编号"累计，不是跳变次数——
    只有这样才能和网关报的丢弃行数对同一件事的账）。

    给 gate 就不读，直到 gate.set()：慢客户端靠这个机制实现，而不是靠另一个线程去抢它的 fd。
    rcvbuf 不为空时在 connect 前把接收缓冲压小（理由见 main 里的注释）。"""

    def __init__(self, port, name, sub=True, raw=False, gated=False, rcvbuf=None):
        super().__init__(daemon=True)
        self.port, self.name, self.want_sub = port, name, sub
        self.want_raw = raw
        self.rcvbuf = rcvbuf
        self.gate = threading.Event() if gated else None
        self.n = 0                # 收到的 SAMPLE 行数
        self.missing = 0          # seq 跳号累计（丢掉的样本数）
        self.jumps = 0            # 跳号事件次数
        self.other = []           # 非 SAMPLE 行（应答等），供"命令不被丢"判定
        self.err = None
        self.last = None
        self.stop = False
        self.sock = None
        self.started = threading.Event()
        self.t_first = self.t_last = None

    def run(self):
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if self.rcvbuf:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf)
            s.settimeout(5)
            s.connect(("127.0.0.1", self.port))
            s.settimeout(2)
            self.sock = s
            if self.want_sub:
                s.sendall(b"SUBSCRIBE\n")
            elif self.want_raw:
                s.sendall(b"RAW\n")        # 字节流订阅者：它不读行，只把队列顶满
            self.started.set()
            if self.gate is not None:
                self.gate.wait(60)          # 不读阶段：内核接收缓冲与网关侧队列依次被顶满
            partial = ""
            while not self.stop:
                try:
                    chunk = s.recv(8192)
                except socket.timeout:
                    continue
                except OSError as e:
                    self.err = "recv:%s" % e
                    break
                if not chunk:
                    self.err = "EOF"
                    break
                text = partial + chunk.decode("utf-8", "replace")
                lines = text.split("\n")
                partial = lines.pop()
                for ln in lines:
                    self.feed(ln)
        except BaseException as e:            # 线程里的静默异常会把"没缺口"读成假绿
            self.err = "%s:%s" % (type(e).__name__, e)
            self.started.set()

    def feed(self, ln):
        m = SAMPLE_RE.match(ln) if ln.startswith("SAMPLE ") else None
        if not m:
            if ln:
                self.other.append(ln)
            return
        self.n += 1
        now = time.time()
        if self.t_first is None:
            self.t_first = now
        self.t_last = now
        seq = int(m.group(1))
        if self.last is not None:
            exp = (self.last + 1) % SEQ_MOD
            if seq != exp:
                self.jumps += 1
                d = (seq - self.last - 1) % SEQ_MOD
                self.missing += d if d < SEQ_MOD // 2 else 0
        self.last = seq

    def peer_closed(self):
        """只在它的线程还在 gate 里睡着时从主线程探一下：读到 0 字节/报错 = 网关已收掉，
        EWOULDBLOCK 或有数据 = 连接还活着。"""
        if self.sock is None:
            return False
        try:
            self.sock.setblocking(False)
            return not self.sock.recv(64)
        except BlockingIOError:
            return False
        except OSError:
            return True

    def rates(self):
        """按自己真正在收的时间算速率，不把 connect 与 stop 尾巴算进分母。"""
        span = (self.t_last - self.t_first) if (self.t_first and self.t_last) else 0.0
        return self.n / span if span > 0.5 else 0.0


def ask(port, cmd, timeout=3.0):
    """一次性命令（订阅态之外的管理面读法）。超时返回 None，不抛。"""
    try:
        s = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        s.settimeout(timeout)
        s.sendall((cmd + "\n").encode())
        buf = b""
        while True:
            try:
                d = s.recv(4096)
            except socket.timeout:
                break
            if not d:
                break
            buf += d
            if buf.endswith(b"\n"):
                break
        s.close()
        return buf.decode("utf-8", "replace")
    except OSError:
        return None


def stats_ok(port):
    txt = ask(port, "STATS")
    if not txt:
        return None
    m = re.search(r"STATS ok=(\d+)", txt)
    return int(m.group(1)) if m else None


def start_pair(gw_path, sim_path, logdir, qcap, sndbuf):
    pre = [] if IS_WIN else ["stdbuf", "-oL"]     # 不加则 printf 全缓冲，进程被打掉就丢在缓冲区里
    gwf = open(os.path.join(logdir, "slowprobe_gw.log"), "w", encoding="utf-8")
    simf = open(os.path.join(logdir, "slowprobe_sim.log"), "w", encoding="utf-8")
    sim = subprocess.Popen(pre + [sim_path, str(P_SIM)], stdout=simf, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    gw = subprocess.Popen(pre + [gw_path, str(P_GW), "127.0.0.1", str(P_SIM),
                                 "--qcap", str(qcap), "--sndbuf", str(sndbuf)],
                          stdout=gwf, stderr=subprocess.STDOUT)
    time.sleep(1.0)
    return sim, gw, simf, gwf


def stop_pair(sim, gw, fps):
    for p in (gw, sim):
        if p.poll() is None:
            p.terminate()
            try:
                p.wait(timeout=3)
            except subprocess.TimeoutExpired:
                p.kill()
    for f in fps:
        try:
            f.close()
        except OSError:
            pass


def ports_clear():
    # 不做全局 pkill：本探针的 argv 里就带着 "build_s1/gateway_service" 这个串，
    # 上轮那种中括号断串也挡不住——pkill -f 匹配的是**包含本脚本在内的整条命令行**，
    # 于是探针把自己杀了（本轮实测：EXIT=143/SIGTERM、输出 0 字节）。
    # 只靠两条安全手段：① 开跑前确认端口无人听（孤儿占了就判无效格、不动）；
    # ② 收尾只 terminate 本脚本自己 spawn 的两个子进程。
    for pt in (P_SIM, P_GW):
        try:
            socket.create_connection(("127.0.0.1", pt), timeout=0.3).close()
            print("FAIL 前置: 端口 %d 已有人听，现场不干净，本臂不动" % pt)
            return False
        except OSError:
            pass
    return True


def rcname(rc):
    if rc is None:
        return "存活"
    if rc < 0:
        try:
            return "信号%s" % signal.Signals(-rc).name
        except ValueError:
            return "信号%d" % -rc
    return "码%d" % rc


class Flood(threading.Thread):
    """只发 STATS 不读，把应答堆成"队列里全是不可丢的 Command" → 网关应踢除它。"""

    def __init__(self, port, cap, rcvbuf=None):
        super().__init__(daemon=True)
        self.port, self.cap = port, cap
        self.rcvbuf = rcvbuf
        self.connected = threading.Event()   # 主线程要等它真连上再往下走，理由见 main
        self.wrote = 0
        self.closed_by_gw = False
        self.stop = False
        self.err = None

    def run(self):
        s = None
        try:
            s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
            if self.rcvbuf:
                s.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, self.rcvbuf)
            s.settimeout(5)
            s.connect(("127.0.0.1", self.port))
            s.setblocking(False)
            self.connected.set()             # 只有连上了才发号；否则 accept 顺序会和这里的假设错开
            while self.wrote < self.cap and not self.stop and not self.closed_by_gw:
                try:
                    s.sendall(b"STATS\n")
                    self.wrote += 1
                except BlockingIOError:
                    time.sleep(0.001)              # 我方发送缓冲满：等网关把它读走
                except OSError as e:
                    self.err, self.closed_by_gw = str(e), True
                    break
                if self.wrote % 50 == 0:
                    self.probe_close(s)
            # 写完不主动关：一关就分不清“被踢除”与“自己走了”（本轮就是这么丢判据的）
            while not self.stop and not self.closed_by_gw:
                time.sleep(0.02)
                self.probe_close(s)
        except BaseException as e:
            self.err = "%s:%s" % (type(e).__name__, e)
        finally:
            if s is not None:
                try:
                    s.close()
                except OSError:
                    pass
    
    def probe_close(self, s):
        """非阻塞探一下对端是否已关：读到 0 字节 = 网关已把它收掉；ECONNRESET 也算。"""
        try:
            if not s.recv(64):
                self.closed_by_gw = True
        except BlockingIOError:
            pass
        except OSError as e:
            self.err, self.closed_by_gw = str(e), True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--gw", required=True, help="gateway_service 可执行文件路径")
    ap.add_argument("--sim", required=True, help="device_simulator 可执行文件路径")
    ap.add_argument("--healthy", type=int, default=4, help="健康订阅者路数")
    ap.add_argument("--stall", type=float, default=8.0,
                    help="第几秒给慢客户端下一条 STATS（默认 8s，此时队列应已顶满，"
                         "应答只能挤样本）；窗口不足时自动提前到 window-2s")
    ap.add_argument("--window", type=float, default=50.0,
                    help="测量窗口秒数。两臂用**同一个**窗口长度（上轮给两臂配了不同窗口，"
                         "配对就不成立了）。50s 是三个需求里最慢那个定的：RAW 腿按 240 B/s 要 ≈34s "
                         "才顶满 8KB 队列；对照臂（阻塞 sendAll）要 ≈30s 才能把内核发送缓冲堆到上限")
    ap.add_argument("--flood-cap", type=int, default=300000,
                    help="洪水客户端最多发多少条 STATS。默认值很大是有意的：对照臂（阻塞式 sendAll）"
                         "在本机要把 ≈wmem_max=4MB 的应答（约 100k 条 ×40B）堆完才会真阻塞；"
                         "上轮用 40000 条（≈1.6MB 应答）全部被内核吞掉，对照臂 J2/J3 全绿——那才是真的危险。"
                         "修复臂在 ≈1.4k 条就被踢除（实测），这个 cap 对它无影响。")
    ap.add_argument("--qcap", type=int, default=8192,
                    help="网关每连接出向队列上限；默认 64KB 需要 ≥45s 不读才顶满，探针把它压小")
    ap.add_argument("--rcvbuf", type=int, default=2048,
                    help="慢/洪水客户端的 SO_RCVBUF；见下方剂量算术")
    ap.add_argument("--sndbuf", type=int, default=2048,
                    help="网关每连接 SO_SNDBUF；不设时内核会把它长到 MB 级")
    # 剂量算术（本机 Linux 环回实测，不是内核传说；复算脚本见提交信息）：
    #   对端一个字节不读时能连写多少字节才 EWOULDBLOCK：
    #     默认(不设)          → 内核读回 2626560/131072 → 可吞 2,703,360 B
    #     请求 16384 / 8192   → 读回   32768/ 16384     → 可吞    36,864 B
    #     请求  4096 / 4096   → 读回    8192/  8192     → 可吞    10,240 B
    #     请求  2048 / 2048   → 读回    4608/  4096     → 可吞     4,096 B  ← 默认档
    #   setsockopt 的值内核会翻倍，且有 SOCK_MIN_* 兜底，所以按“请求值×2”估会偏。
    #   一条订阅流约 20 行/s × 66 B ≈ 1.3 KB/s，12s 窗口 ≈ 15.8 KB：
    #   4 KB 内核余量下队列需容纳 ≈ 11.8 KB > qcap 8192 → 必撞上限。
    #   上一轮用 16384/8192（可吞 36 KB > 全部产出）就得到过 drop=0——那不是
    #   “缺陷不存在”而是“探针剂量不够”（B-55 同族：取证手段自身的失效模式）。
    ap.add_argument("--logdir", default=None)
    a = ap.parse_args()
    gw_path, sim_path = os.path.abspath(a.gw), os.path.abspath(a.sim)
    logdir = a.logdir or os.path.dirname(gw_path)
    os.makedirs(logdir, exist_ok=True)

    print("平台=%s  网关=%s  模拟器=%s" % ("Windows" if IS_WIN else "Linux", gw_path, sim_path))
    print("规模: 健康订阅 %d 路 + 慢文本订阅者 1 路 + 应答洪水 1 路 + RAW 慢订阅者 1 路 + 管理面若干次查询；窗口 %.1fs；网关 --qcap %d --sndbuf %d / 慢客户端 SO_RCVBUF %d"
          % (a.healthy, a.window, a.qcap, a.sndbuf, a.rcvbuf))
    if not ports_clear():
        return 2
    gwlog_path = os.path.join(logdir, "slowprobe_gw.log")

    def gwlog_text():
        try:
            return open(gwlog_path, encoding="utf-8", errors="replace").read()
        except OSError:
            return ""

    sim, gw, simf, gwf = start_pair(gw_path, sim_path, logdir, a.qcap, a.sndbuf)
    verdicts = []

    def judge(tag, ok, detail):
        verdicts.append((tag, ok))
        print("  [%s] %-28s %s" % ("PASS" if ok else "FAIL", tag, detail), flush=True)

    try:
        healthy = []
        for i in range(a.healthy):
            r = Reader(P_GW, "healthy%d" % (i + 1))
            r.start()
            r.started.wait(6)
            healthy.append(r)
        slow = Reader(P_GW, "slow", gated=True, rcvbuf=a.rcvbuf)   # 不读，直到窗口末开闸
        slow.start()
        slow.started.wait(6)
        if slow.sock:                       # 把内核读回值打出来：剂量算错时这一行就是证据
            print("  [剂量] 慢客户端 SO_RCVBUF 请求 %d → 内核实际 %d"
                  % (a.rcvbuf, slow.sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF)), flush=True)
        flood = Flood(P_GW, a.flood_cap, rcvbuf=a.rcvbuf)
        flood.start()
        # 必须在任何 ask() 之前确认洪水客户端已连上：id 是按 accept 顺序发的，而
        # flood.start() 返回时那个线程可能还没 connect——下面紧接着的 stats_ok() 就会把
        # id 抢走（Windows 上实测就是这样：ask 拿到 id=6，洪水排到 id=7，J8 按 id=6 找踢除记录找不到）。
        if not flood.connected.wait(6):
            print("  [无效格] 洪水客户端 6s 内没连上，后面的 id 推定全部作废")
        # RAW 慢订阅者：它的队列开了“不挤位”策略，预期是被踢除而不是在帧中间丢一块（J10）
        rawc = Reader(P_GW, "rawslow", sub=False, raw=True, gated=True, rcvbuf=a.rcvbuf)
        rawc.start()
        rawc.started.wait(6)

        ok0 = stats_ok(P_GW)
        t0 = time.time()
        t_stats = min(a.stall, max(a.window - 2.0, 1.0))
        time.sleep(t_stats)                 # 此刻队列应已被样本顶满（stalled=1）
        # 在“队列已满”的当口下一条 STATS：应答只能挤掉样本才装得下（保留优先级）。
        # 放在窗口开头则应答轻松插队，evict=0，J7 就退化成“反正后面会挤掉几次”。
        if slow.sock:
            try:
                slow.sock.sendall(b"STATS\n")
            except OSError as e:
                slow.err = "slow-send:%s" % e
        rest = a.window - (time.time() - t0)
        if rest > 0:
            time.sleep(rest)                # 窗口余下时间慢客户端仍一直不读（gate 未开）
        el = time.time() - t0
        mid_clients = ask(P_GW, "CLIENTS") or ""
        ok1 = stats_ok(P_GW)                 # 产出计数在窗口末就取，不把开闸恢复读的积压时间算进分母
        # 收窗口：停健康路 → 开闸让慢客户端把积压读干净（它的 seq 缺口要从这里量）
        for r in healthy:
            r.stop = True
        slow.gate.set()
        flood.stop = True
        for r in healthy:
            r.join(timeout=6)
        slow.join(timeout=12)
        flood.join(timeout=12)
        end_clients = ask(P_GW, "CLIENTS") or ""
        gw_alive = gw.poll() is None

        def parse_clients(txt):
            out = {}
            for m in CLIENT_RE.finditer(txt):
                v = [int(x) for x in m.groups()]
                out[v[0]] = dict(sub=v[1], raw=v[2], qbytes=v[3], lines=v[4],
                                 stalled=v[5], enq=v[6], sent=v[7], drop=v[8],
                                 evict=v[9], kick=v[10])
            return out
        cmid, cend = parse_clients(mid_clients), parse_clients(end_clients)
        # accept 顺序即 id：healthy 1..K，slow K+1，flood K+2，rawslow K+3
        # （管理面每次 ask 新建连接，只在这四个之后发生，不参与判定）
        slow_id = a.healthy + 1
        flood_id = a.healthy + 2
        raw_id = a.healthy + 3
        hids = list(range(1, a.healthy + 1))

        def cleanup_of(cid):
            """从网关日志里找该连接被收掉那行的三个计数（两条打印格式：recv 路径多一个
            "recv=-n, " 前缀，这里一并接住）。拿不到返回 None。"""
            m = re.search(r"client -id=%d \((?:recv=-?\d+, )?drop=(\d+) evict=(\d+) kick=(\d+)" % cid,
                          gwlog_text())
            if not m:
                return None
            return dict(drop=int(m.group(1)), evict=int(m.group(2)), kick=int(m.group(3)))

        print("\n=== 判据（网关结束时=%s）===" % rcname(gw.poll()))
        judge("J1 网关存活", gw_alive,
              "结束时=%s；CLIENTS 应答 %d 字节" % (rcname(gw.poll()), len(end_clients)))
        rate0 = (ok1 - ok0) / max(el - 1.0, 1.0) if (ok0 is not None and ok1 is not None) else None
        if ok0 is None:
            judge("J2 健康订阅者速率不退化", False, "开局就问不到 STATS（ok0=None）—— 探针无效格，本臂不采信")
            judge("J3 健康订阅者零缺口", False, "同上，基线未立")
        elif ok1 is None:
            # 窗口末问不到 STATS 不是“探针无效”而是本次要抓的病灶本身：
            # 单线程事件循环卡在某个客户端的阻塞 send 里，连管理面都不应答了。
            judge("J2 健康订阅者速率不退化", False,
                  "窗口末 STATS 无应答（ok0=%s → 事件循环已 wedged）——健康各路实测 %.1f..%.1f 行/s"
                  % (ok0, min([r.rates() for r in healthy] or [0]), max([r.rates() for r in healthy] or [0])))
            gaps = [r.missing for r in healthy]
            # 注意：事件循环被卡住时 seq **不会**出现缺口（不是丢，是整个停摊），
            # 所以“零缺口”单独看是假绿，必须与 J2 的速率合读——本行把行数一并打出来。
            judge("J3 健康订阅者零缺口", False,
                  "各路 seq 缺口 %s / 共收到 %s 行——无缺口是因为几乎没收到数据，网关已不应答"
                  % (gaps, [r.n for r in healthy]))
        elif rate0:
            rates = [r.rates() for r in healthy]
            worst = min(rates)
            judge("J2 健康订阅者速率不退化", worst >= 0.8 * rate0,
                  "网关产出 %.1f 帧/s；健康各路 %.1f..%.1f 行/s（最低路 / 产出 = %.2f，阈值 0.8）"
                  % (rate0, min(rates), max(rates), worst / rate0))
            gaps = [r.missing for r in healthy]
            judge("J3 健康订阅者零缺口", all(g == 0 for g in gaps),
                  "各路 seq 缺口 %s，共收到 %s 行（%d 路）" % (gaps, [r.n for r in healthy], len(gaps)))
            alive_ids = [i for i in hids if i in cend]
            judge("J9 健康连接全程不掉线", len(alive_ids) == a.healthy,
                  "窗口末 CLIENTS 里仍在的 healthy id=%s / 期望 %s；读侧异常=%s"
                  % (alive_ids, hids, [r.err for r in healthy if r.err]))
        else:
            # ok0/ok1 都取到了但 rate0 为 0：窗口内网关一帧未产出 = 上游采集停摆，也是真失败
            judge("J2 健康订阅者速率不退化", False,
                  "窗口内网关产出增量为 0（ok0=%s ok1=%s，el=%.1fs）——上游采集停摆" % (ok0, ok1, el))

        cs = cend.get(slow_id) or cmid.get(slow_id)
        sm = cmid.get(slow_id)
        if cs:
            # drop/evict 是累计量，取窗口末那份；但 stalled/qbytes 是**状态量**，必须在
            # “开闸前”那份快照上量——恢复读之后队列被排空，拿窗口末读就会看到
            # stalled=0 / qbytes=0（本轮就是这么把一次真实的顶满误判成 FAIL 的）。
            judge("J4 代价归因到慢客户端自己",
                  cs["drop"] > 0 and sm is not None and sm["stalled"] == 1 and sm["qbytes"] > 0,
                  "累计 drop=%d evict=%d；不读期间(id=%d) stalled=%d qbytes=%d lines=%d，"
                  "恢复读后排空到 qbytes=%d" %
                  (cs["drop"], cs["evict"], slow_id,
                   sm["stalled"] if sm else -1, sm["qbytes"] if sm else -1,
                   sm["lines"] if sm else -1, cs["qbytes"]))
            hs = [cend.get(i) for i in hids if i in cend]
            hm = [cmid.get(i) for i in hids if i in cmid]
            judge("J5 三档计数分离(健康路 0 丢)",
                  all(c["drop"] == 0 and c["stalled"] == 0 for c in hs + hm),
                  "健康各路 drop=%s stalled=%s（两份快照都取：不读期间 + 窗口末）"
                  % ([c["drop"] for c in hs + hm], [c["stalled"] for c in hs + hm]))
            judge("J6 下游 seq 缺口独立复核", abs(slow.missing - cs["drop"]) <= max(4, 0.05 * cs["drop"]),
                  "慢客户端自报缺 %d 个 seq vs 网关报 drop=%d（差 %d，容差 max(4, 5%%)）"
                  % (slow.missing, cs["drop"], abs(slow.missing - cs["drop"])))
            got_stats = any(ln.startswith("STATS ok=") for ln in slow.other)
            judge("J7 命令应答不被丢(挤样本保应答)", got_stats and cs["evict"] > 0,
                  "第 %.1fs（队列已满时）发的 STATS 在恢复读后收到=%s，其间挤掉 evict=%d 行；共读到 %d 行样本"
                  % (t_stats, got_stats, cs["evict"], slow.n))
        else:
            judge("J4 代价归因到慢客户端自己", False,
                  "CLIENTS 里没有 id=%d（它被踢了？窗口末 id=%s）" % (slow_id, sorted(cend)))

        fk = cleanup_of(flood_id)
        judge("J8 应答洪水客户端被踢除",
              fk is not None and fk["kick"] >= 1 and flood.closed_by_gw,
              "网关日志 %s；洪水侧观察到关闭=%s（写入 %d 条，异常=%s）"
              % ("drop=%d evict=%d kick=%d" % (fk["drop"], fk["evict"], fk["kick"]) if fk
                 else "里找不到 id=%d 的收掉记录" % flood_id,
                 flood.closed_by_gw, flood.wrote, flood.err))
        # J10：RAW 字节流腿的“宁可踢除也不丢一块”（方案 T1.1 定案 B-2）。它给 keepStreamIntact
        # 供了一个不是文本行能给的形状：evict 必须为 0，否则就是在帧中间打了一个洞。
        rk = cleanup_of(raw_id)
        raw_closed = rawc.peer_closed()
        judge("J10 RAW 慢订阅者被踢除而非被打洞",
              rk is not None and rk["kick"] >= 1 and rk["evict"] == 0 and raw_closed,
              "id=%d 网关日志 %s；客户端观察到关闭=%s（evict 不为 0 就是在帧中间丢了东西）"
              % (raw_id,
                 "drop=%d evict=%d kick=%d" % (rk["drop"], rk["evict"], rk["kick"]) if rk
                 else "里没有它的收掉记录",
                 raw_closed))
        print("\n窗口中 CLIENTS（慢客户端仍不读时，应当看到 stalled=1 / qbytes>0）：\n"
              + "".join("    " + ln + "\n" for ln in mid_clients.splitlines()))
        print("\n窗口末 CLIENTS：\n" + "".join("    " + ln + "\n" for ln in end_clients.splitlines()))
    finally:
        stop_pair(sim, gw, (simf, gwf))
    npass = sum(1 for _, ok in verdicts if ok)
    print("=== 合计 %d 格，PASS %d，FAIL %d ===" % (len(verdicts), npass, len(verdicts) - npass))
    return 0 if npass == len(verdicts) else 1


if __name__ == "__main__":
    sys.exit(main())
