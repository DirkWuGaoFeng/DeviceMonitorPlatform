// dmp_ros2_bridge/gateway_client.cpp — POSIX TCP 实现 (ROS2 侧只跑 Linux/VM)
#include "dmp_ros2_bridge/gateway_client.hpp"

#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

namespace dmpbr {

namespace {
void setRecvTimeout(int fd, int ms) {
    timeval tv{};
    tv.tv_sec  = ms / 1000;
    tv.tv_usec = (ms % 1000) * 1000;
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
}
} // namespace

GatewayClient::~GatewayClient() { closeAll(); }

void GatewayClient::setEndpoint(std::string host, uint16_t port) {
    host_ = std::move(host);
    port_ = port;
}

int GatewayClient::connectNew() {
    int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port   = htons(port_);
    if (::inet_pton(AF_INET, host_.c_str(), &sa.sin_addr) != 1) {
        // 非点分十进制: 只接受 localhost 作为等价写法, 显式回环地址 (不把 0.0.0.0 当目的地址)
        if (host_ == "localhost" || host_.empty()) {
            if (::inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) { ::close(fd); return -1; }
        } else {
            ::close(fd);
            return -1;
        }
    }
    // 阻塞 connect 的等待上限由内核 SYN 重试决定 (VM 实测: 同网段不可达 3.11s, NAT 后黑洞 21.03s),
    // 而本函数是在调用方的**回调里**被跑的 (tickLink / 三个服务) —— 不封顶就等于把
    // "一个网络故障冻结整个容器" 写进了产品行为。所以: 非阻塞 connect + poll 定时。
    if (connTimeoutMs_ == 0) {
        // 旧行为, 只留给取证脚本做"修与不修"的对照 (生产上不应传 0)
        if (::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa)) != 0) {
            ::close(fd);
            return -1;
        }
    } else {
        const int fl = ::fcntl(fd, F_GETFL, 0);
        if (fl < 0 || ::fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) { ::close(fd); return -1; }
        const int cr = ::connect(fd, reinterpret_cast<sockaddr*>(&sa), sizeof(sa));
        if (cr != 0 && errno != EINPROGRESS) { ::close(fd); return -1; }   // 立即失败(如 ECONNREFUSED)
        if (cr != 0) {                                                    // 还在握手中: 等 poll 上限
            pollfd pf{};
            pf.fd = fd;
            pf.events = POLLOUT;
            int pr;
            // 把总预算算成 deadline 而不是每次 poll 都给满额: 否则"被信号打断就重试"
            // 会变成重试多少次就再等多少秒, 口头上的上限又不是上限了。
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(connTimeoutMs_);
            for (;;) {
                const auto rem = std::chrono::duration_cast<std::chrono::milliseconds>(
                                     deadline - std::chrono::steady_clock::now()).count();
                if (rem <= 0) { pr = 0; break; }                       // 预算用尽 = 超时
                pr = ::poll(&pf, 1, static_cast<int>(rem));
                if (pr >= 0 || errno != EINTR) break;
            }
            if (pr == 0) { ++connTimeouts_; ::close(fd); return -1; }     // 超时: 计一次
            int soerr = 0;
            socklen_t len = sizeof(soerr);
            if (pr < 0 || ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &soerr, &len) != 0 || soerr != 0) {
                ::close(fd);
                return -1;
            }
        }
        // 恢复阻塞: 后面的 sendAll / recvLine / 读线程都按"阻塞 + SO_RCVTIMEO"的口径写的,
        // 把非阻塞语义泄进去会得到"recv 返回 EAGAIN 当成对端关闭"这类错判。
        if (::fcntl(fd, F_SETFL, fl) < 0) { ::close(fd); return -1; }
    }
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));  // 帧流低延迟: 禁用 Nagle
    setRecvTimeout(fd, 200);                            // 让读线程能周期性检查 run_
    return fd;
}

bool GatewayClient::sendAll(int fd, const std::string& s) {
    size_t off = 0;
    while (off < s.size()) {
        ssize_t n = ::send(fd, s.data() + off, s.size() - off, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            return false;
        }
        off += static_cast<size_t>(n);
    }
    return true;
}

bool GatewayClient::recvLine(int fd, std::string& buf, std::string& outLine, int waitMs) {
    for (;;) {
        const size_t nl = buf.find('\n');
        if (nl != std::string::npos) {                       // 缓冲区里已有完整行, 优先消费
            outLine = buf.substr(0, nl + 1);
            buf.erase(0, nl + 1);
            return true;
        }
        timeval tv{};
        tv.tv_sec  = waitMs / 1000;
        tv.tv_usec = (waitMs % 1000) * 1000;
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        const int sr = ::select(fd + 1, &rf, nullptr, nullptr, &tv);
        if (sr < 0) { if (errno == EINTR) continue; return false; }
        if (sr == 0) return false;                           // 静默期到 => 应答结束
        char tmp[2048];
        ssize_t n = ::recv(fd, tmp, sizeof(tmp), 0);
        if (n > 0) { buf.append(tmp, tmp + n); continue; }
        if (n < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        return false;                                        // n==0: 对端关闭
    }
}

bool GatewayClient::ensureData(const DataFn& onData) {
    if (fdData_.load() >= 0) return true;
    if (reader_.joinable()) reader_.join();                  // 回收上一轮已退出的读线程

    int fd = connectNew();
    if (fd < 0) return false;

    std::string buf, line;
    if (!sendAll(fd, "RAW\n") || !recvLine(fd, buf, line, 1000) || line.rfind("+RAW", 0) != 0) {
        ::close(fd);
        return false;
    }
    fdData_.store(fd);
    run_.store(true);
    ++reconnects_;

    reader_ = std::thread([this, fd, onData]() {
        uint8_t chunk[4096];
        while (run_.load()) {
            ssize_t n = ::recv(fd, chunk, sizeof(chunk), 0);
            if (n > 0) {
                onData(chunk, static_cast<size_t>(n));
            } else if (n == 0) {
                break;                                        // 网关关闭
            } else if (errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK) {
                break;                                        // 真错误
            }
        }
        fdData_.store(-1);
        ::close(fd);
    });
    return true;
}

std::string GatewayClient::requestOne(const std::string line) {
    std::lock_guard<std::mutex> lk(ctlMtx_);
    if (fdCtl_ < 0) {
        fdCtl_ = connectNew();
        if (fdCtl_ < 0) return {};
        ctlBuf_.clear();
    }
    if (!sendAll(fdCtl_, line + "\n")) {
        ::close(fdCtl_); fdCtl_ = -1; ctlBuf_.clear();
        return {};
    }
    std::string out;
    if (!recvLine(fdCtl_, ctlBuf_, out, 1000)) {
        ::close(fdCtl_); fdCtl_ = -1; ctlBuf_.clear();
        return {};
    }
    return out;
}

std::string GatewayClient::requestLines(const std::string line, const std::string& prefix) {
    std::lock_guard<std::mutex> lk(ctlMtx_);
    if (fdCtl_ < 0) {
        fdCtl_ = connectNew();
        if (fdCtl_ < 0) return {};
        ctlBuf_.clear();
    }
    if (!sendAll(fdCtl_, line + "\n")) {
        ::close(fdCtl_); fdCtl_ = -1; ctlBuf_.clear();
        return {};
    }
    std::string all, first;
    if (!recvLine(fdCtl_, ctlBuf_, first, 1000)) {
        ::close(fdCtl_); fdCtl_ = -1; ctlBuf_.clear();
        return {};
    }
    all += first;
    // 首行是错误/空应答时没有后续; 否则按前缀继续收, 直到静默期
    if (first.rfind(prefix, 0) == 0) {
        for (;;) {
            std::string more;
            if (!recvLine(fdCtl_, ctlBuf_, more, quietMs_)) break;
            if (more.rfind(prefix, 0) != 0) { all += more; break; }  // 末行可能是 "RULE none\n" 之类
            all += more;
        }
    }
    return all;
}

void GatewayClient::closeAll() {
    run_.store(false);
    if (reader_.joinable()) reader_.join();
    if (fdData_.load() >= 0) { ::close(fdData_.load()); fdData_.store(-1); }
    if (fdCtl_ >= 0)         { ::close(fdCtl_); fdCtl_ = -1; }
    ctlBuf_.clear();
}

} // namespace dmpbr
