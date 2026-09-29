#!/usr/bin/env python3
"""s2_serial_pty.py — Linux termios 串口上游（S2 / SR-016）的端到端实测。仅 Linux。

要证的事（不是"编译过了"）：`--serial <tty>` 在 POSIX 真能把一个 tty 设备节点当上游，
经 termios raw 打开、readSerial() 读进字节、再从 RAW 订阅口原样透传出来。

用 pty.openpty() 造一对伪终端：master 由本脚本持有（扮演 STM32 往里灌字节），
slave 的名字（/dev/pts/N）交给网关 `--serial`。pty slave 支持 termios，能真走通
cfmakeraw/tcsetattr 那条路径——这不是"读到 ENOTTY 就算打开"。

用法（从仓库根、Linux 原生或 WSL 内跑；WSL 里 /dev/pts 可建 pty）：
  python3 tools/diagnostics/s2_serial_pty.py --gw=build_s1/gateway_service

三格判据（每格都问"如果这句话是假的，这一格会给出什么读数"）：
  G1 端口识别：--serial COM4 必须在 POSIX 拒开（rc=2 + "不识别端口"），
     而不是静默映射到某个 /dev/ttyS*——伪映射的症状是"打开了错误的设备"。
  G2 波特率守门：--serial <tty> 12345 必须拒开（rc=2 + "不支持的波特率"），
     而不是静默降到 115200。
  G3 读路径透传（核心，带阳性对照）：网关起来后 TCP 连得上（连得上本身就说明
     openSerial 成功——失败会在 listen 前 return 2）；RAW 订阅后往 master 灌若干段
     可辨识字节，RAW 客户端必须收到同样的字节。readSerial 若恒返回 0（termios 没配好/
     非阻塞没生效），这里就一个字节都收不到 → 当场红，不会被误读成绿。

与 slow_client_probe.py 同属"分钟级时序/端到端"验收，不进 CI 门禁（依方案 §8 第 5 条），走本地实跑。
"""
import os
import pty
import select
import socket
import subprocess
import sys
import time

GW = "build_s1/gateway_service"          # 可用 --gw=<path> 覆盖
GWPORT = 59501
BLOB = b"\xA5SERIALPATH\x01\x02HELLO-FROM-TTY\x03\n"   # 不含 "+RAW"，避免与握手回显混读

PASS, FAIL = 0, 0
def check(name, ok, detail=""):
    global PASS, FAIL
    if ok: PASS += 1; print(f"  [PASS] {name}  {detail}")
    else:  FAIL += 1; print(f"  [FAIL] {name}  {detail}")
    return ok

def port_free(port):
    s = socket.socket()
    try:
        s.bind(("127.0.0.1", port)); s.close(); return True
    except OSError:
        s.close(); return False

def run_reject(args, want_substr):
    """跑一次网关、期望它非零退出并打印含 want_substr 的错误。返回 (rc, out)。"""
    p = subprocess.run([GW] + args, capture_output=True, text=True, timeout=5)
    out = (p.stdout or "") + (p.stderr or "")
    return p.returncode, out

def main():
    global GW
    for a in sys.argv[1:]:
        if a.startswith("--gw="): GW = a.split("=", 1)[1]
    if os.name == "nt":
        print("本测仅 Linux（termios）；Windows 串口腿由既有实跑覆盖"); return 0
    if not os.path.exists(GW):
        print(f"找不到网关 {GW}（先在 Linux 构建，或用 --gw= 指定）"); return 3
    # 端口预检：被占就拒跑并报错，不复用全局 pkill（复盘录 B-56：pkill -f 会连调用者一起杀）
    for p in (GWPORT, GWPORT - 1, GWPORT - 2):
        if not port_free(p):
            print(f"端口 {p} 已被占用，先腾出来再跑（脚本不替你 kill）"); return 3

    master, slave = pty.openpty()
    tty = os.ttyname(slave)
    print(f"虚拟串口: master fd={master}  slave={tty}")

    # --- G1: COM4 在 POSIX 必须被拒，不做伪映射 ---
    rc, out = run_reject([str(GWPORT - 1), "--serial", "COM4"], "不识别端口")
    check("G1 COM4 被拒(不做伪映射)", rc == 2 and "不识别端口" in out, f"rc={rc}")
    if rc != 2 or "不识别端口" not in out:
        print("      实际输出:", out.strip()[:200])

    # --- G2: 不支持的波特率必须被拒 ---
    rc, out = run_reject([str(GWPORT - 2), "--serial", tty, "12345"], "不支持的波特率")
    check("G2 非法波特率被拒(不静默降级)", rc == 2 and "不支持的波特率" in out, f"rc={rc}")

    # --- G3: 打开 tty 成功 + RAW 透传（阳性对照：readSerial 恒 0 时这里收不到字节） ---
    proc = subprocess.Popen([GW, str(GWPORT), "--serial", tty, "115200"],
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    try:
        # 给起来一点时间；若 openSerial 失败会在 listen 前 return 2 → 这里 poll() 非 None
        time.sleep(0.5)
        alive = proc.poll() is None
        if not alive:
            head = proc.stdout.read() if proc.stdout else ""
            print("      网关启动即退出:", head[:200])
        # TCP 连得上本身即证明已越过 openSerial 进到 listen
        try:
            c = socket.create_connection(("127.0.0.1", GWPORT), timeout=5)
        except OSError as e:
            check("G3a 连上网关(=openSerial 已成功)", False, f"connect 失败: {e}")
            return summarize()
        check("G3a 连上网关(=openSerial 已成功)", alive, f"alive={alive}")

        c.settimeout(5)
        c.sendall(b"RAW\n")
        # 收 +RAW 回显
        buf = b""
        t0 = time.time()
        while b"+RAW" not in buf and time.time() - t0 < 5:
            try: buf += c.recv(4096)
            except socket.timeout: break
        got_ack = b"+RAW" in buf
        check("G3b RAW 订阅握手", got_ack, f"首包={buf[:24]!r}")

        # 往 master（=设备侧）灌可辨识字节，分多次、跨过 20ms 轮询节拍
        received = bytearray()
        deadline = time.time() + 2.5
        wrote = 0
        c.setblocking(False)
        while time.time() < deadline:
            os.write(master, BLOB); wrote += 1
            time.sleep(0.05)
            r, _, _ = select.select([c], [], [], 0.02)
            if r:
                try: received += c.recv(65536)
                except (BlockingIOError, socket.timeout): pass
        copies = received.count(BLOB)
        # 阳性对照：灌了 wrote 段，RAW 侧至少该收到其中的多数（最后一段可能尚未被轮询取走）
        check("G3c tty→RAW 字节透传", wrote >= 20 and copies >= 10,
              f"写入 {wrote} 段, RAW 侧收到 {copies} 段完整副本, 共 {len(received)} 字节")

        c.close()
    finally:
        proc.terminate()
        try: proc.wait(timeout=3)
        except subprocess.TimeoutExpired: proc.kill()
        # 不 kill 别人——只收本脚本 spawn 的这一个子进程（复盘录 B-56 的教训在此落地）

    return summarize()

def summarize():
    print(f"\n=== S2 termios 串口实测: {PASS} PASS / {FAIL} FAIL ===")
    return 0 if FAIL == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
