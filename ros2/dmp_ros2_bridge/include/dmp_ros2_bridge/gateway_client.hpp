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
//
// connect 超时 (本文件的命门, 2026-09-29 实测后才加的):
//   connect() 发生在**调用方的回调里** (tickLink 定时器、set_rule 服务回调都走它),
//   而默认的阻塞 connect 等待上限由内核 SYN 重试决定 —— VM 实测: 同网段不可达 IP 3.11s、
//   NAT 后的黑洞地址 21.03s。也就是说: 一次网络故障就能把整个容器冻结那么久,
//   而 IEC 侧 R-003 写的是"任何单个回调必须短" —— 那时它只是一句约定, 不是实现。
//   现在用非阻塞 connect + poll() 显式封顶 (setConnectTimeoutMs), 0 = 退回旧行为,
//   只留给取证脚本做"修与不修"的 A/B 对照。
//   注: 读路径本来就有界 (首行 1000ms、后续行 quietMs=80), 所以黑洞端点造不出长回调 ——
//   会造长回调的只有 connect。这一点实测前我猜错了。
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
    // 单次 connect 的等待上限 (ms); 0 = 不设限 (即旧行为, 只留给取证对照实验)
    void setConnectTimeoutMs(int ms) { connTimeoutMs_ = ms < 0 ? 0 : ms; }

    // 数据面: 连接 + 发送 "RAW\n" + 等待 "+RAW\n", 然后起读线程持续回调。幂等。
    bool ensureData(const DataFn& onData);

    // 控制面: 一行命令 -> 单行应答 (STATS/HELP/RULE_ACK/ERR)。失败返回空串。
    std::string requestOne(const std::string line);

    // 控制面: 一行命令 -> 多行应答, 按前缀+静默期收全 (RULES/ALARMS)。返回含换行的原文。
    std::string requestLines(const std::string line, const std::string& prefix);

    void closeAll();

    bool dataUp() const { return fdData_.load() >= 0; }
    uint64_t reconnects() const { return reconnects_.load(); }
    // connect 被超时顶掉的次数 —— 它是"长回调已被消除"的直接证据, 不用从时间倒推
    uint64_t connectTimeouts() const { return connTimeouts_.load(); }

private:
    int  connectNew();                          // 建立一条新连接, 返回 fd 或 -1
    bool sendAll(int fd, const std::string& s);
    // 从 fd 读满一行 (含 '\n'); waitMs 内读不到返回 false (buf 保留半行以便续读)
    bool recvLine(int fd, std::string& buf, std::string& outLine, int waitMs);

    std::string  host_ = "127.0.0.1";
    uint16_t     port_ = 9100;
    int          quietMs_ = 80;
    int          connTimeoutMs_ = 1000;   // 默认 1s: 远大于同机房正常建连, 远小于内核 SYN 重试
    std::atomic<uint64_t> connTimeouts_{0};

    std::atomic<int> fdData_{-1};
    int              fdCtl_ = -1;
    std::string      ctlBuf_;

    std::thread      reader_;
    std::atomic<bool> run_{false};
    std::atomic<uint64_t> reconnects_{0};
    std::mutex       ctlMtx_;        // 控制面串行化 (服务回调可来自同一执行器, 仍设防)
};

} // namespace dmpbr
