// tests/test_storage.cpp — 存储/回放 的单元+集成测试 (无外部依赖)
//
// 覆盖: CSV 落盘→读回一致性; 表头仅写一次; 多次打开追加不重复表头; 时间戳顺序保持。
// 对应 IEC 可追溯 SR-004「历史存储可追溯」。
#include "dmp/storage.h"

#include <cstdio>
#include <cmath>
#include <string>
#include <vector>

static int g_fail = 0;
static int g_run  = 0;
#define CHECK(cond)                                                          \
    do { ++g_run; if (!(cond)) { std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); ++g_fail; } } while (0)
#define SECTION(name) std::printf("== %s ==\n", name)

using namespace dmp;

static Sample mkS(uint64_t ts, uint16_t seq, uint8_t ch, SampleType t, float v) {
    Sample s; s.recv_ts = ts; s.seq = seq; s.channel = ch; s.type = t; s.value = v; return s;
}

// 测试产物落在当前工作目录 (不依赖子目录存在: 写死 "build/" 在 ctest/Linux 下 fopen 会失败)
static const char* kPath = "_dmp_storage_test.csv";
static const char* kPath2 = "_dmp_storage_test2.csv";

static void test_write_read_roundtrip() {
    SECTION("CSV 写入/读回 一致性");
    std::remove(kPath);
    {
        CsvSink sink; CHECK(sink.open(kPath));
        std::vector<Sample> wrote;
        wrote.push_back(mkS(1000, 1, 0, TYPE_TEMPERATURE, 36.5f));
        wrote.push_back(mkS(1010, 2, 1, TYPE_HEART_RATE,  72.0f));
        wrote.push_back(mkS(1020, 3, 2, TYPE_SPO2,        98.0f));
        for (const auto& s : wrote) sink.onSample(s);
        sink.flush();
        auto read = loadCsv(kPath);
        CHECK(read.size() == wrote.size());
        for (size_t i = 0; i < read.size() && i < wrote.size(); ++i) {
            CHECK(read[i].recv_ts  == wrote[i].recv_ts);
            CHECK(read[i].seq       == wrote[i].seq);
            CHECK(read[i].channel   == wrote[i].channel);
            CHECK(read[i].type      == wrote[i].type);
            CHECK(std::abs(read[i].value - wrote[i].value) < 1e-3f);
        }
    }
    std::remove(kPath);
}

static void test_header_once_on_append() {
    SECTION("多次打开追加: 表头仅一次 & 行数累加");
    std::remove(kPath2);
    { CsvSink a; CHECK(a.open(kPath2)); a.onSample(mkS(1, 1, 0, TYPE_TEMPERATURE, 1.f)); }
    { CsvSink b; CHECK(b.open(kPath2)); b.onSample(mkS(2, 2, 0, TYPE_TEMPERATURE, 2.f)); }
    { CsvSink c; CHECK(c.open(kPath2)); c.onSample(mkS(3, 3, 0, TYPE_TEMPERATURE, 3.f)); }
    auto read = loadCsv(kPath2);
    CHECK(read.size() == 3);                              // 只 3 行数据
    bool ascending = true;
    for (size_t i = 1; i < read.size(); ++i) if (read[i].recv_ts < read[i-1].recv_ts) ascending = false;
    CHECK(ascending);
    std::remove(kPath2);
}

static void test_tee_sink() {
    SECTION("TeeSink: 一份数据扇出到两个 Sink");
    struct VecSink : public Sink { std::vector<Sample> got;
        void onSample(const Sample& s) override { got.push_back(s); } };
    VecSink v1, v2;
    TeeSink tee; tee.add(&v1); tee.add(&v2);
    tee.onSample(mkS(10, 5, 3, TYPE_CONCENTR, 7.7f));
    tee.onSample(mkS(11, 6, 3, TYPE_CONCENTR, 8.8f));
    CHECK(v1.got.size() == 2 && v2.got.size() == 2);
    CHECK(v1.got.size() == 2 && std::abs(v1.got[0].value - 7.7f) < 1e-3f);
}

static void test_bad_line_tolerance() {
    SECTION("坏行容错: 手动写坏行, loadCsv 跳过且不崩");
    std::remove(kPath);
    {
        FILE* fp = std::fopen(kPath, "wb");
        CHECK(fp != nullptr);                      // 无法建文件则后续无意义
        if (fp) {
            std::fprintf(fp, "recv_ts,seq,channel,type,value\n");
            std::fprintf(fp, "100,1,0,1,36.6\n");
            std::fprintf(fp, "bad line here\n");
            std::fprintf(fp, "200,2,1,2,72,,\n"); // 多余逗号 → 解析失败但被跳过
            std::fprintf(fp, "300,3,2,3,98.5\n");
            std::fclose(fp);
        }
    }
    auto read = loadCsv(kPath);
    CHECK(read.size() >= 2);   // 至少两条有效行
    if (!read.empty()) CHECK(read.front().value == 36.6f);   // 空容器上 front() 是 UB
    std::remove(kPath);
}

static void test_pipeline_to_csv() {
    SECTION("集成: 帧→解码→Acquisition→Tee→CSV→回放 (确定性, 不走 socket)");
    struct NullSink : public Sink { void onSample(const Sample&) override {} } null;
    std::remove(kPath);
    {
        CsvSink csv; CHECK(csv.open(kPath));
        TeeSink tee; tee.add(&null); tee.add(&csv);
        Acquisition acq; acq.setSink(&tee);
        std::vector<uint8_t> stream;
        for (int i = 0; i < 5; ++i) {
            Sample s; s.seq = static_cast<uint16_t>(i); s.channel = static_cast<uint8_t>(i % 4);
            s.type = TYPE_TEMPERATURE; s.value = 36.f + i;
            auto f = encodeFrame(s);
            stream.insert(stream.end(), f.begin(), f.end());
        }
        acq.pushBytes(stream.data(), stream.size(), 555);
        size_t n = acq.drain();
        CHECK(n == 5);
        CHECK(acq.stat().ok == 5);
        csv.flush();                         // 落盘后再读 (CsvSink 为吞吐按批 flush)
        auto read = loadCsv(kPath);
        CHECK(read.size() == 5);
        for (const auto& r : read) CHECK(r.recv_ts == 555);   // 接收时间戳正确传递
    }
    std::remove(kPath);
}

int main() {
    test_write_read_roundtrip();
    test_header_once_on_append();
    test_tee_sink();
    test_bad_line_tolerance();
    test_pipeline_to_csv();
    std::printf("\n%s  断言 %d 项, 失败 %d 项\n",
                g_fail == 0 ? "ALL PASS" : "HAS FAILURES", g_run, g_fail);
    return g_fail == 0 ? 0 : 1;
}
