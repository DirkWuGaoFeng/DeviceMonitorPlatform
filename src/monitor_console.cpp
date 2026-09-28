// src/monitor_console.cpp — 纯 C++ 控制台监控端 (不依赖 Qt, 端到端可跑)
//
// 作用: 连接 device_simulator (或真实设备网关), 用 dmp::Acquisition 解码,
//       在终端打印各通道实时值 + ASCII 条形图 + 危急值告警。
// 意义: 让你在没有 Qt 环境时也能立刻验证"协议→采集→告警"链路是否跑通,
//       是招牌项目的第 1 块可运行砖。
//
// 用法:
//   终端A:  ./device_simulator 9000
//   终端B:  ./monitor_console [host] [port]      默认 127.0.0.1 9000
#include "dmp/acquisition.h"
#include "dmp/storage.h"
#include "dmp/pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

#ifdef _WIN32
  #include <winsock2.h>
  #include <ws2tcpip.h>
  using sock_t = SOCKET;
  #define CLOSESOCK closesocket
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  using sock_t = int;
  #define CLOSESOCK ::close
  #define INVALID_SOCKET (-1)
#endif

// 终端 Sink: 缓存最新值并画 ASCII 条 (演示用; 阈值/量程按仪器配置)
class ConsoleSink : public dmp::Sink {
public:
    void onSample(const dmp::Sample& s) override {
        last_[s.channel] = s.value;
        seen_[s.channel] = true;
    }
    // 每通道显示量程 (仅用于画条形图)
    float lo[4] = { 30.f, 40.f, 90.f, 0.f };
    float hi[4] = { 45.f, 160.f, 100.f, 12.f };

    void render(uint64_t ok, uint64_t crcErr, uint64_t dropped) {
        std::printf("\r\x1b[K");                       // 清行
        const char* name[4] = { "TEMP", "HR  ", "SPO2", "CONC" };
        for (int ch = 0; ch < 4; ++ch) {
            float v = last_[ch];
            std::printf("%s=%6.1f ", name[ch], seen_[ch] ? v : 0.f);
            int width = 12;
            int filled = 0;
            if (seen_[ch] && hi[ch] > lo[ch]) {
                float r = (v - lo[ch]) / (hi[ch] - lo[ch]);
                if (r < 0) r = 0; if (r > 1) r = 1;
                filled = static_cast<int>(r * width + 0.5f);
            }
            for (int i = 0; i < width; ++i) std::printf("%s", i < filled ? "\xe2\x96\x88" : " ");
            std::printf("  ");
        }
        std::printf("| ok=%llu crcErr=%llu drop=%llu",
                     (unsigned long long)ok, (unsigned long long)crcErr,
                     (unsigned long long)dropped);
        std::fflush(stdout);
    }

private:
    float last_[4] = {0};
    bool  seen_[4] = {false, false, false, false};
};

static const char* typeName(dmp::SampleType t) {
    switch (t) {
        case dmp::TYPE_TEMPERATURE: return "体温";
        case dmp::TYPE_HEART_RATE:  return "心率";
        case dmp::TYPE_SPO2:        return "血氧";
        case dmp::TYPE_CONCENTR:    return "浓度";
        default:                    return "量";
    }
}

int main(int argc, char** argv) {
    std::string host = (argc > 1) ? argv[1] : "127.0.0.1";
    int port = (argc > 2) ? std::atoi(argv[2]) : 9000;

#ifdef _WIN32
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock_t sock = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
#ifdef _WIN32
    addr.sin_addr.s_addr = inet_addr(host.c_str());   // 点分十进制即可
#else
    inet_pton(AF_INET, host.c_str(), &addr.sin_addr);
#endif
    std::printf("[monitor] connecting %s:%d ...\n", host.c_str(), port);
    if (::connect(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        std::fprintf(stderr, "connect failed (先启动 device_simulator?)\n");
        return 1;
    }
    std::printf("[monitor] connected. Ctrl+C 退出\n");

    ConsoleSink sink;
    dmp::CsvSink csv;                       // 可选: 第三个参数为导出 CSV 路径
    bool csvOn = false;
    if (argc > 3) {
        if (csv.open(argv[3])) { csvOn = true; std::printf("[monitor] CSV 导出 -> %s\n", argv[3]); }
        else std::fprintf(stderr, "[monitor] 无法打开 CSV 导出文件, 已忽略\n");
    }
    dmp::TeeSink tee; tee.add(&sink); if (csvOn) tee.add(&csv);

    dmp::Acquisition acq;
    acq.setSink(&tee);
    acq.addRule({dmp::TYPE_HEART_RATE, 50.f, 110.f, "心率超出危急范围"});
    acq.addRule({dmp::TYPE_TEMPERATURE, 35.f, 42.f, "体温超出范围"});
    acq.addRule({dmp::TYPE_SPO2, 94.f, 100.f, "血氧偏低"});
    std::atomic<bool> alarmed{false};
    acq.setAlarmCallback([](const dmp::AlarmEvent& e) {
        // 独立于热刷新行的告警输出
        std::printf("\n\x1b[7m\x1b[31m ALARM \x1b[0m %s %s: %s = %.2f  (CH%u, seq=%u)\n",
                    "\xe2\x9a\xa0", typeName(e.sample.type), e.message.c_str(),
                    e.sample.value, e.sample.channel, e.sample.seq);
        std::fflush(stdout);
    });

    // 生产: 阻塞式 socket 读取放独立线程(带接收超时, 避免卡住渲染); 消费: 主线程周期 drain。
#ifdef _WIN32
    DWORD rcvTv = 50;   // ms
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rcvTv), sizeof(rcvTv));
#endif
    auto source = [sock](void* b, size_t cap) -> int {
        int n = ::recv(sock, reinterpret_cast<char*>(b), static_cast<int>(cap), 0);
        if (n > 0) return n;
#ifdef _WIN32
        int e = WSAGetLastError();
        if (e == WSAETIMEDOUT || e == WSAEWOULDBLOCK) return 0;   // 超时无数据 → 继续轮询
#else
        if (n == 0) return -1;                                     // 对端正常关闭
#endif
        return -1;                                                 // 错误/断开 → 结束
    };
    dmp::ProducerThread<decltype(source)> prod(acq, source);
    prod.start();

    while (!prod.finished()) {
        acq.drain(1024);
        sink.render(prod.ok(), prod.crcErr(), prod.dropped());
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    acq.drain(1024);
    sink.render(prod.ok(), prod.crcErr(), prod.dropped());
    std::printf("\n[monitor] connection closed by peer\n");
    prod.stop();

    CLOSESOCK(sock);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
