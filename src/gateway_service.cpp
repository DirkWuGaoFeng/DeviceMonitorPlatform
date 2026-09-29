// src/gateway_service.cpp — 遥测网关服务 (采集/告警后端服务化, 不依赖 Qt)
//
// 角色: 位于"设备数据源(上游: device_simulator 的 TCP 帧流 或 STM32 串口)"与
//       "多个上位机/客户端(下游)"之间的服务层。它对上游按帧协议解码, 对下游暴露
//       dmp/service_proto.h 定义的文本行 API: HELP / STATS / ALARMS[n] / SUBSCRIBE。
//       即一个"二进制设备帧 -> 文本服务 API"的协议翻译网关 (边缘网关常见形态)。
//
// 单线程 select 事件循环: 一个 Acquisition 只被这一个线程读写(无需 pipeline 线程化)。
// 出向经**每客户端有界队列**中转 (dmp/outbound_queue.h)：广播与应答只塞队列，
// 下游 socket 为非阻塞，真正的 send 集中在每轮循环末尾能写多少写多少——一个不读
// 的公告客户端只能弄丢自己的字节，不能停住整个循环 (对应 SR-014 / 消减 R-009)。
// 未来把本文件的 recv/parse/send 换成 gRPC server (见 proto/telemetry.proto) 即可, 业务层不动。
//
// 运行: 先起模拟器 ./device_simulator 9000, 再起 ./gateway_service 9100 127.0.0.1 9000
//       真机模式: ./gateway_service 9100 --serial COM4 [baud]   (上游=STM32 串口; 订阅者除文本行外
//                 还会收到原始帧字节流 RAW, 供 WSL 侧 dmp_grpc_server 直连做全链路)
// 自测: telnet/nc 127.0.0.1 9100  然后输入 STATS / ALARMS 5 / SUBSCRIBE / CLIENTS / HELP
//
// 日志约定: 只走 stdout。重定向到文件时 CRT 默认全缓冲(Windows 尤其如此，且它的 _IOLBF
//           对磁盘文件不可靠)，所以客户端加/减这几行生命周期日志后都显式 fflush——
//           一个还活着的进程把自己的诊断“留在缓冲区里”等于事后无从查起。
#include "dmp/acquisition.h"
#include "dmp/service_proto.h"
#include "dmp/outbound_queue.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <deque>
#include <string>
#include <vector>

namespace obq = dmp::obq;   // 出向队列的类型前缀（命令词 K 已被 svc::CmdKind 占用，此处不取同名）

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #include <windows.h>
  using sock_t = SOCKET;
  #define CLOSESOCK closesocket
  #define LASTWSAE()  WSAGetLastError()
  // Winsock 对“向已关对端 send”只返回错误(WSAECONNRESET 等)，不产生信号
  static constexpr int SEND_FLAGS = 0;
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>   // inet_addr (Linux/macOS 不随 sys/socket.h 提供)
  #include <unistd.h>
  #include <fcntl.h>       // fcntl / O_NONBLOCK (下游转非阻塞)
  #include <sys/select.h>
  using sock_t = int;
  #define CLOSESOCK ::close
  #define INVALID_SOCKET (-1)
  #define LASTWSAE()  (errno)
  // POSIX 上向已被对端关闭的 socket 写，除了返回 EPIPE 还会 raise(SIGPIPE)，
  // 而它的默认动作是终结整个进程 —— 一个下游掉线会带走所有消费者与上游采集腿。
  // MSG_NOSIGNAL 只关掉“这一次写”的信号；macOS/BSD 无此 flag，靠 main() 里的
  // signal(SIGPIPE, SIG_IGN) 兜底(见下方 SIGPIPE 注释)。两道一起上是故意重复。
  #ifdef MSG_NOSIGNAL
    static constexpr int SEND_FLAGS = MSG_NOSIGNAL;
  #else
    static constexpr int SEND_FLAGS = 0;
  #endif
#endif

static uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// 把 drain 出的样本暂存到 vector, 便于随后向订阅者广播
struct VecSink : dmp::Sink {
    std::vector<dmp::Sample> v;
    void onSample(const dmp::Sample& s) override { v.push_back(s); }
};

struct Client {
    sock_t      sock;
    uint64_t    id = 0;               // 按 accept 顺序发的稳定编号：不拿 vector 下标当身份，否则一有 erase 就张冠李戴
    bool        subscribing = false;
    bool        raw = false;          // 订阅原始帧字节流(与上游逐字节相同), 供下游再解码(gRPC 服务)
    std::string inbuf;      // 行缓冲
    // 出向队列 (T1.1 / SR-014)：广播与应答只往这里塞，真正的 send 集中在每轮循环末尾的 pump()。
    // 默认 64KB 上限≈ 45 s 的公告样本流(20 行/s)：慢客户端的代价被约在它自己这一行上，
    // 不再反过来卡住事件循环。
    obq::OutboundQueue out;
    bool        dead = false;         // 只置标记，实际 close+erase 由主循环末尾的统一清扫完成
};

// “这一次写/读没数据可处理”与“这条链接已死”必须分清：前者是非阻塞的正常空转，
// 后者才该回收。混为一谈的代价是——健康连接会在 select 误报可读时被错杀。
static bool wouldBlock(int err) {
#ifdef _WIN32
    return err == WSAEWOULDBLOCK;
#else
    return err == EAGAIN || err == EWOULDBLOCK;
#endif
}

// 出向队列只在**非阻塞** socket 上有意义：否则慢客户端仍会把 send 卡在的内核发送缓冲
// 里，整个单线程事件循环跟着停摊（就是本轮要消减的 R-009）。
// 只对下游 accepted 连接设置；上游与 listener 不读不写会阻塞在 select 前，行为不变。
static void setNonBlock(sock_t s) {
#ifdef _WIN32
    u_long nb = 1;
    ::ioctlsocket(s, FIONBIO, &nb);
#else
    int fl = ::fcntl(s, F_GETFL, 0);
    if (fl >= 0) ::fcntl(s, F_SETFL, fl | O_NONBLOCK);
#endif
}

// 内核会把发送缓冲按 ACK 一步步长到 wmem_max：本机实测（Linux 环回，对端一个字节都不读，
// 不显式设定）能连写 2,703,360 字节才碰 EWOULDBLOCK（读回 sk_sndbuf=2626560）。不设上限时，
// “有界队列”只约束了应用层那一段，大批积压其实住在内核里。显式压小 SO_SNDBUF 后，
// 队列的上限才是这条连接真正的内存上界。默认 0 = 不改系统默认（部署方按链路调）。
static void setSendBuf(sock_t s, int bytes) {
    if (bytes <= 0) return;
    ::setsockopt(s, SOL_SOCKET, SO_SNDBUF,
                 reinterpret_cast<const char*>(&bytes), static_cast<socklen_t>(sizeof(bytes)));
}

// 生产侧：把一段待发内容按行拆开塞进该客户端队列。拆成单行是为了让丢弃有粒度——
// 整块塞的话一条超过上限的应答会被**整块**拒掉，其中的样本也跟着没了。
// 例外：RAW 字节流订阅者整块没有行边界，它的队列开了 keepStreamIntact——不挤位，装不下就要求踢除。
// 返回 false = 该连接已被置 dead（应答塞不下，或字节流装不下）。
static bool enqueue(Client& c, const std::string& blob, obq::Kind k) {
    size_t off = 0;
    while (off < blob.size()) {
        const size_t nl  = blob.find('\n', off);
        const size_t end = (nl == std::string::npos) ? blob.size() : nl + 1;
        const uint64_t kick0 = c.out.counters().kickNeeded;
        if (!c.out.push(blob.substr(off, end - off), k) &&
            c.out.counters().kickNeeded != kick0) {
            c.dead = true;    // 应答腾不出位：它已经太久没读，留着只会拖垮共上游的其他客户端
            return false;
        }
        off = end;
    }
    return true;
}

// 发送侧：能写多少写多少，写不进就原样留到下一轮（peek 不删，所以重发是免费的）。
static void pump(Client& c) {
    while (!c.out.empty() && !c.dead) {
        const std::string w = c.out.peek(65536);
        const int n = ::send(c.sock, w.data(), static_cast<int>(w.size()), SEND_FLAGS);
        if (n > 0) {
            c.out.commit(static_cast<size_t>(n));
            if (static_cast<size_t>(n) < w.size()) break;   // 内核发送缓冲写满了：剩下的下一轮再说
            continue;
        }
        if (wouldBlock(LASTWSAE())) { c.out.commit(0); break; }  // 空转：不得改动队列与计数
        c.dead = true;                                       // 真错误（对端已关等）
    }
}

#ifdef _WIN32
// ---- 串口上游 (与 monitor_serial 同一套路: "COM4"/"4" → \\.\COM4, 115200 8N1) ----
static HANDLE g_ser = INVALID_HANDLE_VALUE;   // 串口上游句柄 (仅 Windows)
static std::string normalizePort(const std::string& in) {
    std::string s = in;
    if (s.size() > 3 && (s[0] == 'C' || s[0] == 'c') &&
        (s[1] == 'O' || s[1] == 'o') && (s[2] == 'M' || s[2] == 'm'))
        return "\\\\.\\" + s;                 // 已是 COMx
    for (char c : s) if (!(c >= '0' && c <= '9')) return "\\\\.\\" + s;  // 含非数字, 原样
    return "\\\\.\\COM" + s;                  // 纯数字 -> COMx
}
static bool openSerial(const std::string& port, int baud) {
    std::string np = normalizePort(port);
    HANDLE h = CreateFileA(np.c_str(), GENERIC_READ | GENERIC_WRITE,
                           0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) { std::fprintf(stderr, "[gateway] CreateFileA(%s) err=%lu\n", np.c_str(), ::GetLastError()); return false; }
    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 10;
    to.ReadTotalTimeoutMultiplier = 1;
    to.ReadTotalTimeoutConstant = 50;   // ReadFile 最多阻塞 50ms, 不饿死 select 循环
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR);
    g_ser = h;
    return true;
}
#endif

int main(int argc, char** argv) {
    int listenPort   = (argc > 1) ? std::atoi(argv[1]) : 9100;
    const char* upHost = (argc > 2) ? argv[2] : "127.0.0.1";
    int upPort       = (argc > 3) ? std::atoi(argv[3]) : 9000;
    // 真机模式: 上游换成 STM32 串口, 透传原始字节 + 文本行 API
    std::string serPort;
    int serBaud = 115200;
    for (int i = 2; i < argc; ++i)
        if (!std::strcmp(argv[i], "--serial") && i + 1 < argc) {
            serPort = argv[++i];
            if (i + 1 < argc && argv[i + 1][0] != '-') serBaud = std::atoi(argv[++i]);
        }

    // 每连接出向队列上限：默认 64KB（≈ 45 s 公告流）。调小是为了在**小流量短窗口**下也能
    // 真撞到上限（慢客户端探针靠它把丢弃/挤掉/踢除三档在同一轮里跑出来），不是绕开机制。
    size_t qcap = 64 * 1024;
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--qcap")) qcap = static_cast<size_t>(std::atoi(argv[i + 1]));
    if (qcap < 1024) qcap = 1024;              // 下限保护：连一行都装不下的配置会把所有推送都当"超长"丢
    int sndbuf = 0;                            // 每连接 SO_SNDBUF；0 = 不改（见 setSendBuf 注释）
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--sndbuf")) sndbuf = std::atoi(argv[i + 1]);
    obq::Params qp;
    qp.capBytes     = qcap;
    qp.hiWaterBytes = qcap * 3 / 4;
    qp.loWaterBytes = qcap / 4;

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    // 忽略 SIGPIPE 后，send 失败只剩一个返回值 false：队列版的 pump() 看到 n<0 且不是 EAGAIN
    // 就把该连接置 dead，下一轮末尾统一回收；recv 返回 0 也能走同一条 erase 路径（与 Windows 一致）。
    // 实测依据（本轮 Linux，N 路订阅者同时结束后观察 1 s，两轮共24 格）：
    // 修改前 N=2 死 0/6、N=4 死 1/6、N=6 死 5/6、N=8 死 6/6，全为 SIGPIPE；Windows 同脚本 0/12。
    std::signal(SIGPIPE, SIG_IGN);
#endif
    bool serialUp = !serPort.empty();
    sock_t up = INVALID_SOCKET;
    if (serialUp) {
#ifdef _WIN32
        if (!openSerial(serPort, serBaud)) {
            std::fprintf(stderr, "[gateway] 打开串口 %s 失败 (被占用/驱动?)\n", serPort.c_str());
            return 2;
        }
        std::printf("[gateway] 上游 = 串口 %s @ %d (真机模式)\n", serPort.c_str(), serBaud);
#else
        std::fprintf(stderr, "[gateway] --serial 仅支持 Windows 侧\n");
        return 2;
#endif
    } else {
        // 上游: 连接设备数据源 (TCP)
        up = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in ua{}; ua.sin_family = AF_INET; ua.sin_port = htons(static_cast<uint16_t>(upPort));
        ua.sin_addr.s_addr = inet_addr(upHost);
        if (::connect(up, reinterpret_cast<sockaddr*>(&ua), sizeof(ua)) < 0) {
            std::fprintf(stderr, "[gateway] 连不上上游 %s:%d (先启动 device_simulator)\n", upHost, upPort);
            return 2;
        }
    }

    // 监听: 下游客户端
    sock_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));
    sockaddr_in la{}; la.sin_family = AF_INET; la.sin_addr.s_addr = INADDR_ANY;
    la.sin_port = htons(static_cast<uint16_t>(listenPort));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&la), sizeof(la)) < 0) {
        std::fprintf(stderr, "[gateway] bind 端口 %d 失败\n", listenPort);
        return 1;
    }
    ::listen(listener, 16);
    std::printf("[gateway] listening :%d  <- upstream %s:%d\n", listenPort, upHost, upPort);
    std::printf("[gateway] 出向队列上限 = %llu 字节/连接 (高水位 %llu / 低水位 %llu)\n",
                static_cast<unsigned long long>(qp.capBytes),
                static_cast<unsigned long long>(qp.hiWaterBytes),
                static_cast<unsigned long long>(qp.loWaterBytes));
    std::printf("[gateway] 每连接 SO_SNDBUF = %s\n",
                sndbuf > 0 ? "已显式设定" : "未改(取系统默认)");
    if (sndbuf > 0)
        std::printf("[gateway]   SO_SNDBUF = %d 字节\n", sndbuf);

    dmp::Acquisition acq;
    VecSink          sink;
    acq.setSink(&sink);
    std::deque<dmp::AlarmEvent> alarmRing;                 // 最近告警(有界)
    acq.setAlarmCallback([&](const dmp::AlarmEvent& a) {
        alarmRing.push_back(a);
        if (alarmRing.size() > 100) alarmRing.pop_front();
    });
    // 近期样本环: 供 HISTORY 窗口聚合 (12B/帧@5Hz*4ch ≈ 150KB/30000条, 有界)
    std::deque<dmp::Sample> sampleRing;
    static constexpr size_t kSampleCap = 30000;
    // 演示阈值 (与上位机一致): 心率 50..110, 体温 35..42, 血氧 94..100
    acq.addRule({dmp::TYPE_HEART_RATE,  50.f, 110.f, "HR out of range"});
    acq.addRule({dmp::TYPE_TEMPERATURE, 35.f, 42.f,  "TEMP out of range"});
    acq.addRule({dmp::TYPE_SPO2,        94.f, 100.f, "SPO2 low"});

    std::vector<Client> clients;
    uint64_t nextClientId = 1;

    auto onUpstreamBytes = [&](uint8_t* buf, size_t n) {
        acq.pushBytes(buf, n, nowMs());
        acq.drain(1024);
        for (const auto& s : sink.v) {
            std::string line = dmp::svc::formatSample(s);
            for (auto& c : clients) if (c.subscribing && !c.raw) enqueue(c, line, obq::Kind::Sample);
        }
        for (const auto& s : sink.v) {
            sampleRing.push_back(s);
            if (sampleRing.size() > kSampleCap) sampleRing.pop_front();
        }
        sink.v.clear();
    };

    for (;;) {
        fd_set rf; FD_ZERO(&rf);
        FD_SET(listener, &rf);
        if (!serialUp) FD_SET(up, &rf);
        sock_t nf = listener + 1;
        for (auto& c : clients) { FD_SET(c.sock, &rf); if (c.sock + 1 > nf) nf = c.sock + 1; }
        timeval tv{0, serialUp ? 20000 : 100000};   // 串口模式 20ms 轮询一次
        if (::select(static_cast<int>(nf), &rf, nullptr, nullptr, &tv) < 0) break;

        // 上游来数据: 解码 -> drain -> 广播给订阅者 (文本行或 RAW 字节透传)
        if (serialUp) {
#ifdef _WIN32
            uint8_t buf[4096];
            DWORD nr = 0;
            if (ReadFile(g_ser, buf, sizeof(buf), &nr, nullptr) && nr > 0) {
                onUpstreamBytes(buf, nr);
                // RAW 也走队列（出向只有这一个出口）。该队列开了 keepStreamIntact：
                // 宁可踢除也不在帧中间丢一块（方案 T1.1 定案 B-2）——下游桥有重连能力，
                // 被踢会从整帧重新对齐；而一个静默的洞只会把 crc_err 顶成尖峰、污染归因。
                for (auto& c : clients)
                    if (c.raw) enqueue(c, std::string(reinterpret_cast<char*>(buf), nr), obq::Kind::Sample);
            }
#endif
        } else if (FD_ISSET(up, &rf)) {
            uint8_t buf[4096];
            int n = ::recv(up, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) { std::printf("[gateway] 上游断开, 退出\n"); break; }
            onUpstreamBytes(buf, static_cast<size_t>(n));
            for (auto& c : clients)
                if (c.raw) enqueue(c, std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(n)), obq::Kind::Sample);
        }

        // 新客户端
        if (FD_ISSET(listener, &rf)) {
            sock_t c = ::accept(listener, nullptr, nullptr);
            if (c != INVALID_SOCKET) {
                setNonBlock(c);
                setSendBuf(c, sndbuf);
                Client nc{};              // 逐字段取默认值(NSDMI)，sock/id/out 紧接赋值
                nc.sock = c;
                nc.id   = nextClientId++;
                nc.out  = obq::OutboundQueue(qp);
                clients.push_back(nc);
                std::printf("[gateway] client + (id=%llu, now %zu)\n",
                            static_cast<unsigned long long>(nc.id), clients.size());
                std::fflush(stdout);   // 生命周期行必须落盘：工具与事后排障读的就是这几行
            }
        }

        // 客户端命令
        for (auto it = clients.begin(); it != clients.end();) {
            if (!FD_ISSET(it->sock, &rf)) { ++it; continue; }
            char rb[512];
            int n = ::recv(it->sock, rb, sizeof(rb), 0);
            if (n <= 0) {
                // 非阻塞空读不是断开：select 报可读与对端 FIN 之间没有因果，
                // 把 EAGAIN 当成 EOF 会错杀健康连接（本轮探针的“全程 0 掉线”判据盯的就是这一条）。
                if (n < 0 && wouldBlock(LASTWSAE())) { ++it; continue; }
                // 对端正常关闭/出错也要留底：不打印的话，下游只看到“连接没在名单里”，
                // 分不清是被踢除还是自己走的（本轮就是靠这条区分开洪水客户端的下场的）。
                std::printf("[gateway] client -id=%llu (recv=%d, drop=%llu evict=%llu kick=%llu, now %zu)\n",
                            static_cast<unsigned long long>(it->id), n,
                            static_cast<unsigned long long>(it->out.counters().dropLines),
                            static_cast<unsigned long long>(it->out.counters().evictLines),
                            static_cast<unsigned long long>(it->out.counters().kickNeeded),
                            clients.size() - 1);
                std::fflush(stdout);
                CLOSESOCK(it->sock); it = clients.erase(it); continue;
            }
            it->inbuf.append(rb, static_cast<size_t>(n));
            size_t nl;
            while ((nl = it->inbuf.find('\n')) != std::string::npos) {
                std::string line = it->inbuf.substr(0, nl);
                it->inbuf.erase(0, nl + 1);
                auto cmd = dmp::svc::parseCommand(line);
                using K = dmp::svc::CmdKind;
                auto st = acq.stat();
                switch (cmd.kind) {
                    case K::Help:  enqueue(*it, dmp::svc::formatHelp(), obq::Kind::Command); break;
                    case K::Stats: enqueue(*it, dmp::svc::formatStats(st.ok, st.crcErr, acq.dropped()), obq::Kind::Command); break;
                    case K::Clients: {   // 每连接出向队列体检 (慢客户端归因用，见 SR-014)
                        std::string outStr;
                        for (size_t i = 0; i < clients.size(); ++i) {
                            const Client& cl = clients[i];
                            if (cl.dead) continue;
                            dmp::svc::ClientStat cs;
                            cs.id      = cl.id;
                            cs.sub     = cl.subscribing ? 1 : 0;
                            cs.raw     = cl.raw ? 1 : 0;
                            cs.qbytes  = cl.out.bytes();
                            cs.lines   = cl.out.lines();
                            cs.stalled = cl.out.stalled() ? 1 : 0;
                            cs.enqLines  = cl.out.counters().enqLines;
                            cs.sentBytes = cl.out.counters().sentBytes;
                            cs.dropLines = cl.out.counters().dropLines;
                            cs.evictLines = cl.out.counters().evictLines;
                            cs.kickLines  = cl.out.counters().kickNeeded;
                            outStr += dmp::svc::formatClient(cs);
                        }
                        if (outStr.empty()) outStr = "CLIENTS none\n";
                        enqueue(*it, outStr, obq::Kind::Command);
                        break;
                    }
                    case K::Alarms: {
                        size_t start = alarmRing.size() > (size_t)cmd.arg ? alarmRing.size() - cmd.arg : 0;
                        std::string out;
                        for (size_t i = start; i < alarmRing.size(); ++i) out += dmp::svc::formatAlarm(alarmRing[i]);
                        if (out.empty()) out = "ALARM none\n";
                        enqueue(*it, out, obq::Kind::Command);
                        break;
                    }
                    case K::Subscribe: it->subscribing = true; enqueue(*it, "+SUBSCRIBED\n", obq::Kind::Command); break;
                    case K::Raw:   // 订阅原始帧字节流(上游逐字节透传), 供 WSL gRPC 服务直连再解码
                        it->raw = true;
                        // 从此刻起这条队列不再挤位：字节流里丢任意一块都会在帧中间打洞。
                        it->out.requireStreamIntact(true);
                        enqueue(*it, "+RAW\n", obq::Kind::Command); break;
                    case K::History: {   // 近 arg2 秒逐通道聚合 (LLM Agent 告警归因用)
                        uint64_t cutoff = nowMs() - static_cast<uint64_t>(cmd.arg2) * 1000ull;
                        struct Agg { uint64_t cnt = 0; double sum = 0, mn = 1e30, mx = -1e30, last = 0; } aggs[8];
                        for (const auto& s : sampleRing) {
                            if (s.recv_ts < cutoff) continue;
                            if (cmd.arg >= 0 && s.channel != cmd.arg) continue;
                            if (s.channel >= 8) continue;
                            auto& a = aggs[s.channel];
                            ++a.cnt; a.sum += s.value;
                            if (s.value < a.mn) a.mn = s.value;
                            if (s.value > a.mx) a.mx = s.value;
                            a.last = s.value;
                        }
                        std::string out;
                        for (int ch = 0; ch < 8; ++ch)
                            if (aggs[ch].cnt) {
                                dmp::svc::HistAgg h;
                                h.channel = ch; h.count = aggs[ch].cnt;
                                h.avg = aggs[ch].sum / aggs[ch].cnt;
                                h.min = aggs[ch].mn; h.max = aggs[ch].mx; h.last = aggs[ch].last;
                                out += dmp::svc::formatHistory(h);
                            }
                        if (out.empty()) out = "HISTORY empty\n";
                        enqueue(*it, out, obq::Kind::Command);
                        break;
                    }
                    case K::Rules: {   // 配置回读 (下游下发前应先读, 下发后回读核对)
                        std::string out;
                        for (const auto& r : acq.rules()) out += dmp::svc::formatRule(r);
                        if (out.empty()) out = "RULE none\n";
                        enqueue(*it, out, obq::Kind::Command);
                        break;
                    }
                    case K::SetRule: { // 幂等下发: 仅改告警判定阈值, 不碰解码/采样链路
                        if (!std::isfinite(cmd.f1) || !std::isfinite(cmd.f2) || cmd.f1 >= cmd.f2) {
                            char b[128];
                            std::snprintf(b, sizeof(b),
                                          "invalid range: need low<high (got low=%g high=%g)",
                                          static_cast<double>(cmd.f1), static_cast<double>(cmd.f2));
                            enqueue(*it, dmp::svc::formatError(b), obq::Kind::Command);
                            std::printf("[gateway] RULE rejected: %s\n", b);
                            break;
                        }
                        dmp::AlarmRule r;
                        r.type    = static_cast<dmp::SampleType>(cmd.arg);
                        r.low     = cmd.f1;
                        r.high    = cmd.f2;
                        r.message = cmd.text.empty() ? dmp::svc::typeName(r.type) + " out of range" : cmd.text;
                        acq.setRule(r);                       // 同类型覆盖 => 重复下发无副作用
                        enqueue(*it, dmp::svc::formatRuleAck(r), obq::Kind::Command);
                        std::printf("[gateway] RULE applied: type=%d low=%g high=%g msg=%s (rules=%zu)\n",
                                    cmd.arg, static_cast<double>(r.low), static_cast<double>(r.high),
                                    r.message.c_str(), acq.rules().size());
                        break;
                    }
                    case K::Unknown: default:
                        enqueue(*it, dmp::svc::formatError("unknown command: " + cmd.name), obq::Kind::Command);
                        break;
                }
            }
            ++it;
        }

        // 出向统一出口：本轮新塞进来的字节尽量在本轮就发出去；发不掉的留在各自队列里。
        // 然后做一次清扫：dead 只在这里真整 close——上面所有生产路径都在遍历 clients，
        // 就地 erase 会让广播循环的引用失效。
        for (auto& c : clients) pump(c);
        for (auto it = clients.begin(); it != clients.end();) {
            if (!it->dead) { ++it; continue; }
            std::printf("[gateway] client -id=%llu (drop=%llu evict=%llu kick=%llu, now %zu)\n",
                        static_cast<unsigned long long>(it->id),
                        static_cast<unsigned long long>(it->out.counters().dropLines),
                        static_cast<unsigned long long>(it->out.counters().evictLines),
                        static_cast<unsigned long long>(it->out.counters().kickNeeded),
                        clients.size() - 1);
            std::fflush(stdout);
            CLOSESOCK(it->sock); it = clients.erase(it);
        }
    }

    for (auto& c : clients) CLOSESOCK(c.sock);
    CLOSESOCK(up); CLOSESOCK(listener);
#ifdef _WIN32
    if (g_ser != INVALID_HANDLE_VALUE) CloseHandle(g_ser);
    WSACleanup();
#endif
    return 0;
}
