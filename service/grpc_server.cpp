// service/grpc_server.cpp — 遥测服务的 gRPC 实现 (WSL2/Linux 侧)
//
// 定位: 与 src/gateway_service.cpp(文本行 TCP 网关) 同语义的"真 gRPC"版本, 复用同一套
//       可移植核心库 include/dmp/{frame_protocol,acquisition}.h。业务层(Acquisition/告警)不变,
//       仅把"recv/parse/send 文本行"换成 gRPC service 实现。
//
// 数据面: 一个内部线程独占写 dmp::Acquisition(单线程访问, 无需锁); 解出的样本/告警经 mutex
//         保护的有界队列发布给 gRPC handler 线程。统计量以原子快照供 GetStats 读。
//
// 两种上游(命令行选择):
//   合成模式(默认, WSL 内自洽): 后台按 ~20Hz 造 4 通道数据(偶发越界->告警), 不依赖任何外部进程;
//   上游模式: 连 device_simulator 的 TCP 帧流(需与模拟器同网络可达, 见 docs/Linux-gRPC-服务层落地指南.md)。
//
// 运行: ./dmp_grpc_server 50051                 # 合成模式, 监听 :50051
//       ./dmp_grpc_server 50051 127.0.0.1 9000  # 连上游模拟器
#include "dmp/frame_protocol.h"
#include "dmp/acquisition.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <random>

#include <grpcpp/grpcpp.h>
#include "telemetry.grpc.pb.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace std::chrono_literals;

// ---- 跨线程共享的已解码状态 (由 reader 线程写, handler 线程读) ----
struct SharedState {
    std::mutex mu;
    std::condition_variable cv;

    // 单调递增的全局写游标 + 近期样本环(供 Subscribe 追读)
    uint64_t               writeIdx = 0;         // 下一条写入的全局序号
    std::deque<dmp::Sample> ring;                // 近期样本(有界)
    static constexpr size_t kRingCap = 8192;

    std::deque<dmp::AlarmEvent> alarms;          // 近期告警(有界)
    static constexpr size_t kAlarmCap = 200;

    // 原子统计快照 (reader 线程 publish; GetStats 读)
    std::atomic<uint64_t> ok{0}, crcErr{0}, dropped{0};
};

// Acquisition 的 Sink: 把 drain 出的样本发布到 SharedState.ring
class StateSink : public dmp::Sink {
public:
    explicit StateSink(SharedState& st) : st_(st) {}
    void onSample(const dmp::Sample& s) override {
        std::lock_guard<std::mutex> g(st_.mu);
        st_.ring.push_back(s);
        if (st_.ring.size() > SharedState::kRingCap) st_.ring.pop_front();
        ++st_.writeIdx;
        st_.cv.notify_all();
    }
private:
    SharedState& st_;
};

static uint64_t nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

// dmp::SampleType -> proto 枚举 (值 1..5 天然对齐; 0=UNKNOWN)
static dmp::telemetry::Sample_Type toProtoType(dmp::SampleType t) {
    switch (t) {
        case dmp::TYPE_TEMPERATURE: return dmp::telemetry::Sample_Type_TEMPERATURE;
        case dmp::TYPE_HEART_RATE:  return dmp::telemetry::Sample_Type_HEART_RATE;
        case dmp::TYPE_SPO2:        return dmp::telemetry::Sample_Type_SPO2;
        case dmp::TYPE_CONCENTR:    return dmp::telemetry::Sample_Type_CONCENTR;
        case dmp::TYPE_PRESSURE:    return dmp::telemetry::Sample_Type_PRESSURE;
        default:                    return dmp::telemetry::Sample_Type_UNKNOWN;
    }
}

static void fillSample(const dmp::Sample& s, dmp::telemetry::Sample* out) {
    out->set_seq(s.seq);
    out->set_channel(s.channel);
    out->set_type(toProtoType(s.type));
    out->set_value(s.value);
    out->set_ts_ms(s.recv_ts);
}

// ---- gRPC 服务实现 ----
class TelemetryService final : public dmp::telemetry::Telemetry::Service {
public:
    explicit TelemetryService(SharedState& st) : st_(st) {}

    grpc::Status GetStats(grpc::ServerContext*, const dmp::telemetry::Empty*,
                          dmp::telemetry::Stats* resp) override {
        resp->set_ok(st_.ok.load());
        resp->set_crc_err(st_.crcErr.load());
        resp->set_dropped(st_.dropped.load());
        return grpc::Status::OK;
    }

    grpc::Status GetAlarms(grpc::ServerContext* ctx, const dmp::telemetry::AlarmsRequest* req,
                           grpc::ServerWriter<dmp::telemetry::AlarmEvent>* writer) override {
        int limit = req->limit() > 0 ? static_cast<int>(req->limit()) : 10;
        std::deque<dmp::AlarmEvent> snapshot;
        {
            std::lock_guard<std::mutex> g(st_.mu);
            size_t start = st_.alarms.size() > static_cast<size_t>(limit)
                               ? st_.alarms.size() - limit : 0;
            snapshot.assign(st_.alarms.begin() + start, st_.alarms.end());
        }
        for (const auto& a : snapshot) {
            if (ctx->IsCancelled()) break;
            dmp::telemetry::AlarmEvent out;
            fillSample(a.sample, out.mutable_sample());
            out.set_message(a.message);
            out.set_critical(a.critical);
            writer->Write(out);
        }
        return grpc::Status::OK;
    }

    grpc::Status Subscribe(grpc::ServerContext* ctx, const dmp::telemetry::SubscribeRequest* req,
                           grpc::ServerWriter<dmp::telemetry::Sample>* writer) override {
        // 类型过滤 (空 = 全部)。proto 生成的 repeated Type 是 RepeatedField<int>, 按 int 收集。
        std::vector<int> want;
        for (int t : req->types()) want.push_back(t);
        auto accepted = [&](dmp::telemetry::Sample_Type t) {
            if (want.empty()) return true;
            int v = static_cast<int>(t);
            for (int w : want) if (w == v) return true;
            return false;
        };

        std::unique_lock<std::mutex> lk(st_.mu);
        uint64_t cursor = st_.writeIdx;   // 只推订阅之后的新样本
        while (!ctx->IsCancelled()) {
            st_.cv.wait_for(lk, 500ms);
            // 环容量: 若 cursor 落后过远被裁剪, 跳到最旧可读位置
            uint64_t oldest = (st_.writeIdx > st_.ring.size()) ? st_.writeIdx - st_.ring.size() : 0;
            if (cursor < oldest) cursor = oldest;
            while (cursor < st_.writeIdx) {
                size_t idx = static_cast<size_t>(cursor - oldest);
                dmp::Sample s = st_.ring[idx];
                ++cursor;
                auto pt = toProtoType(s.type);
                if (!accepted(pt)) continue;
                dmp::telemetry::Sample out;
                fillSample(s, &out);
                lk.unlock();
                bool okw = writer->Write(out);
                lk.lock();
                if (!okw || ctx->IsCancelled()) return grpc::Status(grpc::StatusCode::CANCELLED, "cancelled");
            }
        }
        return grpc::Status::OK;
    }

private:
    SharedState& st_;
};

// ---- 数据面 reader 线程: 独占写 Acquisition, 发布统计/告警 ----
static std::atomic<bool> g_stop{false};

static void feedAlarm(SharedState& st, const dmp::AlarmEvent& a) {
    std::lock_guard<std::mutex> g(st.mu);
    st.alarms.push_back(a);
    if (st.alarms.size() > SharedState::kAlarmCap) st.alarms.pop_front();
}

// 合成模式: 自造 4 通道 ~20Hz 数据 (心率偶尔越界触发告警)
static void runSynthetic(dmp::Acquisition& acq, SharedState& st) {
    std::mt19937 rng(1234567u);
    uint16_t seq = 0;
    auto next = std::chrono::steady_clock::now();
    while (!g_stop.load()) {
        next += 50ms;   // 20Hz
        // 每拍造 4 通道
        for (int ch = 0; ch < 4; ++ch) {
            dmp::Sample s;
            s.seq = seq++;
            s.channel = static_cast<uint8_t>(ch);
            s.type = static_cast<dmp::SampleType>(ch + 1); // 1..4
            std::uniform_real_distribution<float> jitter(0.f, 1.f);
            switch (s.type) {
                case dmp::TYPE_TEMPERATURE: s.value = 36.5f + jitter(rng) * 0.6f; break;        // 36.5..37.1
                case dmp::TYPE_HEART_RATE:  s.value = 70.f  + jitter(rng) * (jitter(rng) > 0.85f ? 60.f : 10.f); break; // 偶发>110
                case dmp::TYPE_SPO2:        s.value = 97.f  - jitter(rng) * 2.f;  break;         // 95..97, 偶发<94 边界
                case dmp::TYPE_CONCENTR:    s.value = 4.f   + jitter(rng);        break;
                default:                    s.value = 0.f;
            }
            auto bytes = dmp::encodeFrame(s);
            acq.pushBytes(bytes.data(), bytes.size(), nowMs());
        }
        acq.drain(256);
        auto stat = acq.stat();
        st.ok.store(stat.ok); st.crcErr.store(stat.crcErr); st.dropped.store(acq.dropped());
        // 睡到下一拍
        auto now = std::chrono::steady_clock::now();
        if (next > now) std::this_thread::sleep_until(next);
        else next = now;
    }
}

// 上游模式: 连帧流 TCP 源。兼容两种上游: device_simulator(直接发帧),
// gateway_service(先送一行 "RAW\n" 订阅原始字节透传; 模拟器会忽略这行非帧字节)
static void runUpstream(dmp::Acquisition& acq, SharedState& st, const std::string& host, int port) {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(static_cast<uint16_t>(port));
    inet_pton(AF_INET, host.c_str(), &a.sin_addr);
    if (::connect(fd, reinterpret_cast<sockaddr*>(&a), sizeof(a)) < 0) {
        std::fprintf(stderr, "[grpc] 连不上上游 %s:%d\n", host.c_str(), port);
        return;
    }
    const char* sub = "RAW\n";
    ssize_t sw = ::send(fd, sub, 4, 0); (void)sw;   // 对 device_simulator 无害, 对 gateway 开启 RAW 透传
    uint8_t buf[4096];
    while (!g_stop.load()) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n <= 0) break;
        acq.pushBytes(buf, static_cast<size_t>(n), nowMs());
        acq.drain(1024);
        auto stat = acq.stat();
        st.ok.store(stat.ok); st.crcErr.store(stat.crcErr); st.dropped.store(acq.dropped());
    }
    ::close(fd);
}

static void onSignal(int) { g_stop.store(true); }

int main(int argc, char** argv) {
    int port = (argc > 1) ? std::atoi(argv[1]) : 50051;
    std::string upHost = (argc > 2 && std::strcmp(argv[2], "synthetic") != 0) ? argv[2] : "";
    int upPort = (argc > 3) ? std::atoi(argv[3]) : 9000;

    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    SharedState st;
    StateSink sink(st);

    dmp::Acquisition acq;
    acq.setSink(&sink);
    acq.addRule({dmp::TYPE_HEART_RATE,  50.f, 110.f, "HR out of range"});
    acq.addRule({dmp::TYPE_TEMPERATURE, 35.f, 42.f,  "TEMP out of range"});
    acq.addRule({dmp::TYPE_SPO2,        94.f, 100.f, "SPO2 low"});
    acq.setAlarmCallback([&](const dmp::AlarmEvent& a) { feedAlarm(st, a); });

    // 数据面线程
    std::thread reader([&] {
        if (upHost.empty()) runSynthetic(acq, st);
        else                runUpstream(acq, st, upHost, upPort);
    });

    // gRPC 服务端
    TelemetryService service(st);
    std::string addr = "0.0.0.0:" + std::to_string(port);
    grpc::ServerBuilder builder;
    builder.AddListeningPort(addr, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);
    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::fprintf(stderr, "[grpc] 启动失败 (addr=%s)\n", addr.c_str());
        g_stop.store(true); reader.join(); return 1;
    }
    std::printf("[grpc] Telemetry server listening on %s  (mode=%s)\n",
                addr.c_str(), upHost.empty() ? "synthetic" : ("upstream " + upHost + ":" + std::to_string(upPort)).c_str());
    std::fflush(stdout);

    // 监视线程: 收到停止信号 -> 优雅关闭 gRPC (使 Wait() 返回)
    std::thread watcher([&] {
        while (!g_stop.load()) std::this_thread::sleep_for(100ms);
        server->Shutdown();
    });

    server->Wait();
    g_stop.store(true);
    if (watcher.joinable()) watcher.join();
    reader.join();
    return 0;
}
