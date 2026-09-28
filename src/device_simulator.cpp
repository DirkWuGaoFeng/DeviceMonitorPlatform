// src/device_simulator.cpp — "被测仪器"的 TCP 数据源 (核心链路, 不依赖 Qt)
//
// 作用: 在没有真实 STM32/传感器时, 生成 温度/心率/血氧/浓度 的周期性+噪声数据,
//       按 dmp 帧协议编码后, 通过 TCP 广播给所有已连接的上位机 (qt_monitor)。
// 后续: 用 STM32 + 传感器 通过 UART 发同样的帧替换本程序即可。
//
// 编译: cmake --build build --target device_simulator
#include "dmp/frame_protocol.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <unistd.h>
  using sock_t = int;
  #define CLOSESOCK ::close
  #define INVALID_SOCKET (-1)
#endif

static void sleepMs(int ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 9000;

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    int opt = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&opt), sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (::bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "bind failed on port %d\n", port);
        return 1;
    }
    ::listen(listener, 8);
    std::printf("[simulator] listening on :%d  (Ctrl+C 退出)\n", port);

    std::mt19937 rng(42);
    std::uniform_real_distribution<float> jitter(-1.f, 1.f);
    const dmp::SampleType types[] = {
        dmp::TYPE_TEMPERATURE, dmp::TYPE_HEART_RATE, dmp::TYPE_SPO2, dmp::TYPE_CONCENTR
    };
    const float base[]   = { 36.8f, 72.f,  98.f,  5.0f };   // 基线
    const float amp[]    = { 0.4f,  8.f,   1.5f,  0.6f };   // 幅度
    uint16_t seq = 0;
    std::vector<sock_t> clients;

    for (int tick = 0; ; ++tick) {
        // 接受新连接(非阻塞式轮询)
        fd_set rf; FD_ZERO(&rf); FD_SET(listener, &rf);
        timeval tv{0, 0};
        // nfds 语义差异: Windows 的 select 忽略第一个参数, POSIX 要求它是 max_fd+1。
        // 传 0 在 Windows 上跑得好好的, 到 Linux 上就变成"一个 fd 都不检查" —— accept 永不发生,
        // 而三次握手由内核 backlog 完成, 所以症状是"连接正常但零字节", 极难从现象倒推。
#ifdef _WIN32
        const int nfds = 0;
#else
        const int nfds = listener + 1;
#endif
        if (::select(nfds, &rf, nullptr, nullptr, &tv) > 0) {
            sock_t c = ::accept(listener, nullptr, nullptr);
            if (c != INVALID_SOCKET) { clients.push_back(c); std::printf("[simulator] client connected\n"); }
        }

        // 生成一批样本 (每通道一个)
        std::vector<uint8_t> batch;
        for (size_t ch = 0; ch < 4; ++ch) {
            dmp::Sample s;
            s.seq = seq++;
            s.channel = static_cast<uint8_t>(ch);
            s.type = types[ch];
            float v = base[ch] + amp[ch] * std::sin(tick * 0.1 + ch) + 0.3f * jitter(rng);
            if (ch == 1 && tick % 40 == 0) v += 30.f; // 偶发"心率危急值"用于演示告警
            s.value = v;
            auto f = dmp::encodeFrame(s);
            batch.insert(batch.end(), f.begin(), f.end());
        }

        // 广播给所有客户端; 掉线的移除
        for (auto it = clients.begin(); it != clients.end();) {
            int sent = ::send(*it, reinterpret_cast<const char*>(batch.data()),
                              static_cast<int>(batch.size()), 0);
            if (sent <= 0) { CLOSESOCK(*it); it = clients.erase(it);
                             std::printf("[simulator] client dropped\n"); }
            else ++it;
        }

        sleepMs(200); // 5 Hz/通道
    }

    for (auto c : clients) CLOSESOCK(c);
    CLOSESOCK(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
