// src/monitor_serial.cpp — 上位机裸串口监控端 (真实 STM32 数据源, 不依赖 Qt)
//
// 作用: 直接打开 PC 串口 (CH340/CP210x 映射的 COMx), 读原始字节喂给
//       dmp::Acquisition 解码, 终端打印四通道实时值 + ASCII 条 + 危急值告警。
//       与 monitor_console 共用同一套协议/采集/告警/存储库, 只是传输层从 TCP
//       换成 UART —— 这正是"招牌项目接真实硬件"的那块砖。
//
// 用法 (Windows):
//   monitor_serial.exe COM4                 # 默认 115200
//   monitor_serial.exe COM4 115200
//   monitor_serial.exe 4   --csv out.csv    # 省略 COM 前缀亦可; 带历史导出
//   monitor_serial.exe COM4 --csv out.csv
#include "dmp/acquisition.h"
#include "dmp/storage.h"
#include "dmp/pipeline.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#ifdef _WIN32
  #include <windows.h>
#endif

// 终端 Sink: 缓存最新值并画 ASCII 条 (与 monitor_console 同风格)
class ConsoleSink : public dmp::Sink {
public:
    void onSample(const dmp::Sample& s) override {
        last_[s.channel] = s.value;
        seen_[s.channel] = true;
    }
    float lo[4] = { 30.f, 40.f, 90.f, 0.f };
    float hi[4] = { 45.f, 160.f, 100.f, 12.f };

    void render(uint64_t ok, uint64_t crcErr, uint64_t dropped, uint64_t raw) {
        std::printf("\r\x1b[K");
        const char* name[4] = { "TEMP", "HR  ", "SPO2", "CONC" };
        for (int ch = 0; ch < 4; ++ch) {
            float v = last_[ch];
            std::printf("%s=%6.1f ", name[ch], seen_[ch] ? v : 0.f);
            int width = 12, filled = 0;
            if (seen_[ch] && hi[ch] > lo[ch]) {
                float r = (v - lo[ch]) / (hi[ch] - lo[ch]);
                if (r < 0) r = 0; if (r > 1) r = 1;
                filled = static_cast<int>(r * width + 0.5f);
            }
            for (int i = 0; i < width; ++i) std::printf("%s", i < filled ? "\xe2\x96\x88" : " ");
            std::printf("  ");
        }
        std::printf("| raw=%llu ok=%llu crcErr=%llu drop=%llu",
                     (unsigned long long)raw,
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

#ifdef _WIN32
// 把 "COM4" 或 "4" 归一成 "\\\\.\\COM4"
static std::string normalizePort(const std::string& in) {
    std::string s = in;
    if (s.size() > 3 && (s[0] == 'C' || s[0] == 'c') &&
        (s[1] == 'O' || s[1] == 'o') && (s[2] == 'M' || s[2] == 'm')) {
        return "\\\\.\\" + s;                 // 已是 COMx
    }
    for (char c : s) if (!(c >= '0' && c <= '9')) return "\\\\.\\" + s;  // 含非数字, 原样
    return "\\\\." "\\COM" + s;               // 纯数字 -> COMx
}

static bool openSerial(HANDLE& h, const std::string& port, int baud) {
    h = CreateFileA(normalizePort(port).c_str(), GENERIC_READ | GENERIC_WRITE,
                    0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DCB dcb{}; dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }
    dcb.BaudRate = static_cast<DWORD>(baud);
    dcb.ByteSize = 8; dcb.Parity = NOPARITY; dcb.StopBits = ONESTOPBIT;
    dcb.fBinary = TRUE; dcb.fParity = FALSE; dcb.fOutxCtsFlow = FALSE;
    dcb.fOutxDsrFlow = FALSE; dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE; dcb.fAbortOnError = FALSE;
    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }
    COMMTIMEOUTS to{};
    to.ReadIntervalTimeout = 10;     // 字节间 >10ms 视为一包结束
    to.ReadTotalTimeoutMultiplier = 1;
    to.ReadTotalTimeoutConstant = 50; // 让 ReadFile 周期性返回, 便于渲染
    SetCommTimeouts(h, &to);
    PurgeComm(h, PURGE_RXCLEAR);
    return true;
}
#endif

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "用法: %s <COM端口> [波特率] [--csv 文件]\n", argv[0]);
        std::fprintf(stderr, "  例: %s COM4 115200 --csv out.csv\n", argv[0]);
        return 2;
    }
    std::string port = argv[1];
    int baud = 115200;
    std::string csvPath;
    for (int i = 2; i < argc; ++i) {
        std::string a = argv[i];
        if (a == "--csv" && i + 1 < argc) csvPath = argv[++i];
        else baud = std::atoi(a.c_str());
    }

#ifndef _WIN32
    std::fprintf(stderr, "此串口端目前仅实现 Windows 版本。\n");
    return 1;
#else
    HANDLE h = INVALID_HANDLE_VALUE;
    if (!openSerial(h, port, baud)) {
        std::fprintf(stderr, "[serial] 打开 %s@%d 失败 (端口被占用? 权限? STT?\n)",
                     port.c_str(), baud);
        return 1;
    }
    std::printf("[serial] 已打开 %s @ %d 8N1. Ctrl+C 退出\n", port.c_str(), baud);

    ConsoleSink sink;
    dmp::CsvSink csv;
    bool csvOn = false;
    if (!csvPath.empty()) {
        if (csv.open(csvPath)) { csvOn = true; std::printf("[serial] CSV 导出 -> %s\n", csvPath.c_str()); }
        else std::fprintf(stderr, "[serial] 无法打开 CSV 导出文件, 已忽略\n");
    }
    dmp::TeeSink tee; tee.add(&sink); if (csvOn) tee.add(&csv);

    dmp::Acquisition acq;
    acq.setSink(&tee);
    acq.addRule({dmp::TYPE_HEART_RATE, 50.f, 110.f, "心率超出危急范围"});
    acq.addRule({dmp::TYPE_TEMPERATURE, 35.f, 42.f, "体温超出范围"});
    acq.addRule({dmp::TYPE_SPO2, 94.f, 100.f, "血氧偏低"});
    acq.setAlarmCallback([](const dmp::AlarmEvent& e) {
        std::printf("\n\x1b[7m\x1b[31m ALARM \x1b[0m %s %s: %s = %.2f  (CH%u, seq=%u)\n",
                    "\xe2\x9a\xa0", typeName(e.sample.type), e.message.c_str(),
                    e.sample.value, e.sample.channel, e.sample.seq);
        std::fflush(stdout);
    });

    // 生产: 阻塞式串口读取放独立线程; 消费: 主线程只做周期 drain + 渲染, 读不卡界面。
    // rawBytes: 累计从串口读到的原始字节数, 用于区分"物理层无数据"与"有数据但非 DMP 帧"。
    auto rawBytes = std::make_shared<std::atomic<uint64_t>>(0);
    auto source = [h, rawBytes](void* b, size_t cap) -> int {
        DWORD nr = 0;
        if (!ReadFile(h, b, static_cast<DWORD>(cap), &nr, nullptr)) return -1;  // 出错/拔线 → 结束
        if (nr) rawBytes->fetch_add(nr, std::memory_order_relaxed);
        return static_cast<int>(nr);                                             // 超时无数据返回 0
    };
    dmp::ProducerThread<decltype(source)> prod(acq, source);
    prod.start();

    while (!prod.finished()) {
        acq.drain(1024);
        sink.render(prod.ok(), prod.crcErr(), prod.dropped(), rawBytes->load(std::memory_order_relaxed));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    acq.drain(1024);
    sink.render(prod.ok(), prod.crcErr(), prod.dropped(), rawBytes->load(std::memory_order_relaxed));
    std::printf("\n[serial] 数据源结束\n");
    prod.stop();

    CloseHandle(h);
    return 0;
#endif
}
