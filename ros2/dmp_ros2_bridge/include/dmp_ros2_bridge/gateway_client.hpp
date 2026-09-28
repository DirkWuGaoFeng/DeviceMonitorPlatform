// dmp_ros2_bridge/gateway_client.hpp — 遥测网关的 TCP 客户端 (数据面/控制面分离)
//
// 为什么是两条连接:
//   网关对 RAW 订阅者逐字节透传上游二进制帧, 对普通命令回文本行。若把两者放在同一条
//   连接上, "STATS" 的文本应答会插进帧字节流里, 下游无法定界 —— 这不是风格问题而是正确性
//   问题。故: 数据面一条连接只做 RAW 订阅, 控制面另一条连接只走一行一答的命令。
//
// 多行应答的定界 (已知取舍):
//   网关协议没有"应答结束"标记。这里用 [期望前缀] + [静默期] 组合判定结束: 首行到达后,
//   继续接收以同一前缀开头的行, 直到 quietMs 内没有新数据。对 RULES/ALARMS 这类小体量、
//   单段到达的应答足够稳定。更彻底的做法是给协议加 END 帧, 已记为后续改进项 (见 ros2/README.md)。
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

namespace dmpbr {

class GatewayClient {
public:
    using DataFn = std::function<void(const uint8_t* buf, size_t len)>;

    GatewayClient() = default;
    ~GatewayClient();
    GatewayClient(const GatewayClient&) = delete;
    GatewayClient& operator=(const GatewayClient&) = delete;

    void setEndpoint(std::string host, uint16_t port);

    // 数据面: 连接 + 发送 "RAW\n" + 等待 "+RAW\n", 然后起读线程持续回调。幂等。
    bool ensureData(const DataFn& onData);

    // 控制面: 一行命令 -> 单行应答 (STATS/HELP/RULE_ACK/ERR)。失败返回空串。
    std::string requestOne(const std::string line);

    // 控制面: 一行命令 -> 多行应答, 按前缀+静默期收全 (RULES/ALARMS)。返回含换行的原文。
    std::string requestLines(const std::string line, const std::string& prefix);

    void closeAll();

    bool dataUp() const { return fdData_.load() >= 0; }
    uint64_t reconnects() const { return reconnects_.load(); }

private:
    int  connectNew();                          // 建立一条新连接, 返回 fd 或 -1
    bool sendAll(int fd, const std::string& s);
    // 从 fd 读满一行 (含 '\n'); waitMs 内读不到返回 false (buf 保留半行以便续读)
    bool recvLine(int fd, std::string& buf, std::string& outLine, int waitMs);

    std::string  host_ = "127.0.0.1";
    uint16_t     port_ = 9100;
    int          quietMs_ = 80;

    std::atomic<int> fdData_{-1};
    int              fdCtl_ = -1;
    std::string      ctlBuf_;

    std::thread      reader_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> reconnects_{0};
    std::mutex       ctlMtx_;        // 控制面串行化 (服务回调可来自同一执行器, 仍设防)
};

} // namespace dmpbr
