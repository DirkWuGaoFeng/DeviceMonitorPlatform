// dmp/storage.h — 数据持久化: CSV 落库 + 历史回放(读回) + TeeSink
//
// 为什么 CSV 而非直接 SQLite: 库层零外部依赖、跨编译器可跑、可单测、人可读;
// SQLite 版留在 Qt 上位机层(qt_monitor.cpp:SqliteSink)做 GUI 集成。
// 满足 IEC 可追溯 SR-004「历史存储可追溯」在"存储/回放/导出"语义上的闭环。
#pragma once

#include "dmp/frame_protocol.h"
#include "dmp/acquisition.h"   // Sink

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace dmp {

// 把解码出的样本按 CSV 追加落盘 (表头仅在空文件时写一次)
class CsvSink : public Sink {
public:
    ~CsvSink() override { close(); }

    bool open(const std::string& path) {
        path_ = path;
        long sz = 0;
        { std::ifstream probe(path, std::ios::ate); if (probe.good()) sz = (long)probe.tellg(); }
        out_.open(path, std::ios::app);
        if (!out_.is_open()) return false;
        if (sz == 0) out_ << "recv_ts,seq,channel,type,value\n";  // 仅新建时写表头
        return true;
    }

    void onSample(const Sample& s) override {
        if (!out_.is_open()) return;
        out_ << s.recv_ts << ',' << s.seq << ','
             << static_cast<int>(s.channel) << ','
             << static_cast<int>(s.type) << ','
             << s.value << '\n';
        if (++sinceFlush_ >= 64) { flush(); }  // 批量 flush, 兼顾吞吐与掉电窗口
    }

    void flush() override { sinceFlush_ = 0; if (out_.is_open()) out_.flush(); }
    void close() { flush(); if (out_.is_open()) out_.close(); }

private:
    std::string   path_;
    std::ofstream out_;
    unsigned      sinceFlush_ = 0;
};

// 把一份数据同时扇出到多个 Sink (如: 控制台渲染 + CSV 落盘)
class TeeSink : public Sink {
public:
    void add(Sink* s) { kids_.push_back(s); }
    void onSample(const Sample& s) override { for (Sink* k : kids_) k->onSample(s); }
    void flush() override { for (Sink* k : kids_) k->flush(); }
private:
    std::vector<Sink*> kids_;
};

// 历史回放: 从 CSV 读回样本 (跳过表头, 容错坏行)
inline std::vector<Sample> loadCsv(const std::string& path) {
    std::vector<Sample> v;
    std::ifstream in(path);
    if (!in.is_open()) return v;
    std::string line;
    std::getline(in, line);   // 丢弃表头
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::stringstream ss(line);
        std::string a, b, c, d, e;
        if (!std::getline(ss, a, ',')) continue;
        if (!std::getline(ss, b, ',')) continue;
        if (!std::getline(ss, c, ',')) continue;
        if (!std::getline(ss, d, ',')) continue;
        if (!std::getline(ss, e, ',')) continue;
        Sample s;
        try {
            s.recv_ts = static_cast<uint64_t>(std::stoull(a));
            s.seq     = static_cast<uint16_t>(std::stoul(b));
            s.channel = static_cast<uint8_t>(std::stoul(c));
            s.type    = static_cast<SampleType>(std::stoul(d));
            s.value   = std::stof(e);
        } catch (...) { continue; }   // 坏行跳过
        v.push_back(s);
    }
    return v;
}

} // namespace dmp
