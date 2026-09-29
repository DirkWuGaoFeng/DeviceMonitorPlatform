// src/gateway_service.cpp — 遥测网关服务 (采集/告警后端服务化, 不依赖 Qt)
//
// 角色: 位于"设备数据源(上游: device_simulator 的 TCP 帧流 或 STM32 串口)"与
//       "多个上位机/客户端(下游)"之间的服务层。它对上游按帧协议解码, 对下游暴露
//       dmp/service_proto.h 定义的文本行 API: HELP / STATS / ALARMS[n] / SUBSCRIBE。
//       即一个"二进制设备帧 -> 文本服务 API"的协议翻译网关 (边缘网关常见形态)。
//
// 单线程 select 事件循环: 一个 Acquisition 只被这一个线程读写(无需 pipeline 线程化)。
// 未来把本文件的 recv/parse/send 换成 gRPC server (见 proto/telemetry.proto) 即可, 业务层不动。
//
// 运行: 先起模拟器 ./device_simulator 9000, 再起 ./gateway_service 9100 127.0.0.1 9000
//       真机模式: ./gateway_service 9100 --serial COM4 [baud]   (上游=STM32 串口; 订阅者除文本行外
//                 还会收到原始帧字节流 RAW, 供 WSL 侧 dmp_grpc_server 直连做全链路)
// 自测: telnet/nc 127.0.0.1 9100  然后输入 STATS / ALARMS 5 / SUBSCRIBE / HELP
#include "dmp/acquisition.h"
#include "dmp/service_proto.h"

#include <chrono>
#include <cmath>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <vector>

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
  #include <sys/select.h>
  using sock_t = int;
  #define CLOSESOCK ::close
  #define INVALID_SOCKET (-1)
  #define LASTWSAE()  (0)
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
    bool        subscribing = false;
    bool        raw = false;          // 订阅原始帧字节流(与上游逐字节相同), 供下游再解码(gRPC 服务)
    std::string inbuf;      // 行缓冲
};

static bool sendAll(sock_t c, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        int n = ::send(c, s.data() + off, static_cast<int>(s.size() - off), SEND_FLAGS);
        if (n <= 0) return false;
        off += static_cast<size_t>(n);
    }
    return true;
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

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#else
    // 忽略 SIGPIPE 后，send 失败只剩一个返回值 false；本文件目前不依赖它做事，
    // 死链接由下一轮循环里 recv 返回 0 正常走 erase 路径回收（与 Windows 上的行为一致）。
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

    auto onUpstreamBytes = [&](uint8_t* buf, size_t n) {
        acq.pushBytes(buf, n, nowMs());
        acq.drain(1024);
        for (const auto& s : sink.v) {
            std::string line = dmp::svc::formatSample(s);
            for (auto& c : clients) if (c.subscribing && !c.raw) sendAll(c.sock, line);
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
                for (auto& c : clients)
                    if (c.raw) sendAll(c.sock, std::string(reinterpret_cast<char*>(buf), nr));
            }
#endif
        } else if (FD_ISSET(up, &rf)) {
            uint8_t buf[4096];
            int n = ::recv(up, reinterpret_cast<char*>(buf), sizeof(buf), 0);
            if (n <= 0) { std::printf("[gateway] 上游断开, 退出\n"); break; }
            onUpstreamBytes(buf, static_cast<size_t>(n));
            for (auto& c : clients)
                if (c.raw) sendAll(c.sock, std::string(reinterpret_cast<char*>(buf), static_cast<size_t>(n)));
        }

        // 新客户端
        if (FD_ISSET(listener, &rf)) {
            sock_t c = ::accept(listener, nullptr, nullptr);
            if (c != INVALID_SOCKET) { clients.push_back({c, false, {}});
                std::printf("[gateway] client +1 (now %zu)\n", clients.size()); }
        }

        // 客户端命令
        for (auto it = clients.begin(); it != clients.end();) {
            if (!FD_ISSET(it->sock, &rf)) { ++it; continue; }
            char rb[512];
            int n = ::recv(it->sock, rb, sizeof(rb), 0);
            if (n <= 0) { CLOSESOCK(it->sock); it = clients.erase(it); continue; }
            it->inbuf.append(rb, static_cast<size_t>(n));
            size_t nl;
            while ((nl = it->inbuf.find('\n')) != std::string::npos) {
                std::string line = it->inbuf.substr(0, nl);
                it->inbuf.erase(0, nl + 1);
                auto cmd = dmp::svc::parseCommand(line);
                using K = dmp::svc::CmdKind;
                auto st = acq.stat();
                switch (cmd.kind) {
                    case K::Help:  sendAll(it->sock, dmp::svc::formatHelp()); break;
                    case K::Stats: sendAll(it->sock, dmp::svc::formatStats(st.ok, st.crcErr, acq.dropped())); break;
                    case K::Alarms: {
                        size_t start = alarmRing.size() > (size_t)cmd.arg ? alarmRing.size() - cmd.arg : 0;
                        std::string out;
                        for (size_t i = start; i < alarmRing.size(); ++i) out += dmp::svc::formatAlarm(alarmRing[i]);
                        if (out.empty()) out = "ALARM none\n";
                        sendAll(it->sock, out);
                        break;
                    }
                    case K::Subscribe: it->subscribing = true; sendAll(it->sock, "+SUBSCRIBED\n"); break;
                    case K::Raw:   // 订阅原始帧字节流(上游逐字节透传), 供 WSL gRPC 服务直连再解码
                        it->raw = true; sendAll(it->sock, "+RAW\n"); break;
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
                        sendAll(it->sock, out);
                        break;
                    }
                    case K::Rules: {   // 配置回读 (下游下发前应先读, 下发后回读核对)
                        std::string out;
                        for (const auto& r : acq.rules()) out += dmp::svc::formatRule(r);
                        if (out.empty()) out = "RULE none\n";
                        sendAll(it->sock, out);
                        break;
                    }
                    case K::SetRule: { // 幂等下发: 仅改告警判定阈值, 不碰解码/采样链路
                        if (!std::isfinite(cmd.f1) || !std::isfinite(cmd.f2) || cmd.f1 >= cmd.f2) {
                            char b[128];
                            std::snprintf(b, sizeof(b),
                                          "invalid range: need low<high (got low=%g high=%g)",
                                          static_cast<double>(cmd.f1), static_cast<double>(cmd.f2));
                            sendAll(it->sock, dmp::svc::formatError(b));
                            std::printf("[gateway] RULE rejected: %s\n", b);
                            break;
                        }
                        dmp::AlarmRule r;
                        r.type    = static_cast<dmp::SampleType>(cmd.arg);
                        r.low     = cmd.f1;
                        r.high    = cmd.f2;
                        r.message = cmd.text.empty() ? dmp::svc::typeName(r.type) + " out of range" : cmd.text;
                        acq.setRule(r);                       // 同类型覆盖 => 重复下发无副作用
                        sendAll(it->sock, dmp::svc::formatRuleAck(r));
                        std::printf("[gateway] RULE applied: type=%d low=%g high=%g msg=%s (rules=%zu)\n",
                                    cmd.arg, static_cast<double>(r.low), static_cast<double>(r.high),
                                    r.message.c_str(), acq.rules().size());
                        break;
                    }
                    case K::Unknown: default:
                        sendAll(it->sock, dmp::svc::formatError("unknown command: " + cmd.name));
                        break;
                }
            }
            ++it;
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
