// service/grpc_client_probe.cpp — gRPC 客户端探针 (在 WSL 内验证 server)
//
// 作用: 连 Telemetry server, 依次调用 GetStats -> GetAlarms -> Subscribe(收 N 条后返回),
//       打印结果, 证明"真 gRPC 服务层"跑通 (与文本网关 gw_probe.ps1 对应)。
//
// 运行: ./grpc_client_probe [host:port] [订阅条数]
//       ./grpc_client_probe 127.0.0.1:50051 10
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>
#include "telemetry.grpc.pb.h"

using namespace std::chrono_literals;
using dmp::telemetry::Telemetry;

static const char* typeName(dmp::telemetry::Sample_Type t) {
    switch (t) {
        case dmp::telemetry::Sample_Type_TEMPERATURE: return "TEMP";
        case dmp::telemetry::Sample_Type_HEART_RATE:  return "HR";
        case dmp::telemetry::Sample_Type_SPO2:        return "SPO2";
        case dmp::telemetry::Sample_Type_CONCENTR:    return "CONC";
        case dmp::telemetry::Sample_Type_PRESSURE:    return "PRES";
        default:                                      return "UNK";
    }
}

int main(int argc, char** argv) {
    std::string target = (argc > 1) ? argv[1] : "127.0.0.1:50051";
    int subCount       = (argc > 2) ? std::atoi(argv[2]) : 10;

    auto channel = grpc::CreateChannel(target, grpc::InsecureChannelCredentials());
    auto stub = Telemetry::NewStub(channel);

    // 1) GetStats
    {
        dmp::telemetry::Empty req;
        dmp::telemetry::Stats resp;
        grpc::ClientContext ctx;
        auto st = stub->GetStats(&ctx, req, &resp);
        if (!st.ok()) { std::cerr << "GetStats 失败: " << st.error_message() << "\n"; return 1; }
        std::cout << "[STATS]  ok=" << resp.ok() << " crc_err=" << resp.crc_err()
                  << " dropped=" << resp.dropped() << "\n";
    }

    // 2) GetAlarms (server-streaming, 读若干条)
    {
        dmp::telemetry::AlarmsRequest req; req.set_limit(5);
        grpc::ClientContext ctx;
        auto stream = stub->GetAlarms(&ctx, req);
        dmp::telemetry::AlarmEvent ev;
        int n = 0;
        std::cout << "[ALARMS]";
        while (stream->Read(&ev)) {
            std::cout << "\n  - " << ev.message() << "  ("
                      << typeName(ev.sample().type()) << "=" << ev.sample().value()
                      << " critical=" << ev.critical() << ")";
            ++n;
        }
        if (n == 0) std::cout << " (none yet)";
        std::cout << "\n";
        stream->Finish();
    }

    // 3) Subscribe (server-streaming, 收 subCount 条后 Cancel)
    {
        dmp::telemetry::SubscribeRequest req;   // 空 types => 全部
        grpc::ClientContext ctx;
        auto stream = stub->Subscribe(&ctx, req);
        dmp::telemetry::Sample s;
        std::cout << "[SUBSCRIBE] 接收 " << subCount << " 条实时样本:\n";
        for (int i = 0; i < subCount && stream->Read(&s); ++i) {
            std::cout << "  SAMPLE ts=" << s.ts_ms() << " seq=" << s.seq()
                      << " ch=" << s.channel() << " type=" << typeName(s.type())
                      << " value=" << s.value() << "\n";
        }
        ctx.TryCancel();     // 主动结束服务器流
        auto st = stream->Finish();
        std::cout << "[SUBSCRIBE] 结束 (" << (st.ok() ? "OK" : "cancelled") << ")\n";
    }

    std::cout << "probe done.\n";
    return 0;
}
