/**
 * @file vsoa_cluster_harness.cpp
 * @brief v3.12 M2：6 节点编排 + 故障注入 harness 实现。
 */
#include "vsoa_cluster_harness.h"

#include <arpa/inet.h>
#include <csignal>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>

extern "C" {
#include "dispatch/rtdbd_dispatch.h"
#include "vsoa/vsoa_adapter.h"
}

namespace vsoa_cluster {
namespace {

/* 节点进程内跑的 adapter 配置：uid 固定（harness 用途）。 */
constexpr uint32_t kNodeUid = 1000;

void node_audit(void *, uint32_t, uint32_t, uint32_t, uint32_t, uint64_t, const char *)
{
}

/* 节点业务后端：走**共享**的 transport 无关 dispatch（与 UDS 前端同一函数），
 * 使节点对 PING 等能力的响应与生产形态一致。 */
int node_handler(void *ctx, uint16_t opcode, const uint8_t *req, size_t req_len,
                 uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    (void)ctx;
    return static_cast<int>(rtdbd_dispatch(opcode, req, req_len, resp, resp_cap, resp_len));
}

/* 子进程：跑一个 VSOA adapter（loopback），把实际端口经管道告知父进程，
 * 随后进入事件循环（server 的 accept/握手必须由事件循环驱动，§8.3-3）。 */
[[noreturn]] void node_main(int ready_fd, const std::string &service_id)
{
    rtdbd_vsoa_handler_t handler;
    handler.handle = node_handler;
    handler.ctx = nullptr;
    rtdbd_vsoa_adapter_t *adapter = rtdbd_vsoa_adapter_create(&handler);
    if (!adapter) {
        _exit(2);
    }
    rtdbd_vsoa_adapter_set_audit(adapter, node_audit, nullptr);
    rtdbd_vsoa_adapter_set_default_uid(adapter, kNodeUid);
    /* 预共享 token（§8.3-4）：harness 用固定 token 换身份 */
    rtdbd_vsoa_adapter_add_token(adapter, "harness-token", kNodeUid, 0,
                                 service_id.c_str(), RTDBD_ROLE_PLC);
    rtdbd_vsoa_adapter_set_allow_unauthenticated(adapter, true);

    uint16_t port = 0;
    if (!rtdbd_vsoa_adapter_start(adapter, 0, &port)) {
        _exit(3);
    }
    uint16_t be = port;
    ssize_t w = write(ready_fd, &be, sizeof(be));
    (void)w;
    close(ready_fd);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)rtdbd_vsoa_adapter_fds(adapter, &rfds);
        struct timeval tv;
        tv.tv_sec = 0;
        tv.tv_usec = 200000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            rtdbd_vsoa_adapter_input(adapter, &rfds);
        }
    }
}

/* 探测用：ping 的 RPC 回调。 */
struct PingResult {
    bool replied = false;
    uint8_t status = 0;
};

void on_ping(void *arg, vsoa_client_t *, vsoa_header_t *hdr, vsoa_payload_t *)
{
    PingResult *pr = static_cast<PingResult *>(arg);
    pr->replied = true;
    pr->status = hdr ? hdr->status : 0;
}

} // namespace

Harness::~Harness()
{
    stop();
}

bool Harness::spawn_node(Node &n)
{
    int pipefd[2];
    if (pipe(pipefd) != 0) {
        last_error_ = "pipe failed";
        return false;
    }
    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        last_error_ = "fork failed";
        return false;
    }
    if (pid == 0) {
        close(pipefd[0]);
        node_main(pipefd[1], n.service_id);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ssize_t got = read(pipefd[0], &port, sizeof(port));
    close(pipefd[0]);
    if (got != static_cast<ssize_t>(sizeof(port)) || port == 0) {
        int wstatus = 0;
        waitpid(pid, &wstatus, 0);
        last_error_ = "node failed to report port";
        return false;
    }
    n.pid = pid;
    n.port = port;
    n.alive = true;
    return true;
}

bool Harness::start(int n)
{
    if (n <= 0) {
        last_error_ = "invalid node count";
        return false;
    }
    for (int i = 0; i < n; ++i) {
        Node node;
        node.index = i;
        node.instance = "vsoa_harness_" + std::to_string(getpid()) + "_" + std::to_string(i);
        node.service_id = "svc-" + std::to_string(i);
        if (!spawn_node(node)) {
            stop();
            return false;
        }
        nodes_.push_back(node);
    }
    return true;
}

void Harness::stop()
{
    for (auto &n : nodes_) {
        if (n.pid > 0) {
            kill(n.pid, SIGCONT); /* 暂停态也要收掉 */
            kill(n.pid, SIGKILL);
            int wstatus = 0;
            waitpid(n.pid, &wstatus, 0);
        }
        n.pid = -1;
        n.alive = false;
    }
    nodes_.clear();
}

bool Harness::inject(int index, Fault fault)
{
    if (index < 0 || index >= static_cast<int>(nodes_.size())) {
        last_error_ = "index out of range";
        return false;
    }
    Node &n = nodes_[static_cast<size_t>(index)];
    if (n.pid <= 0) {
        last_error_ = "node not running";
        return false;
    }
    switch (fault) {
        case Fault::Crash:
            if (kill(n.pid, SIGKILL) != 0) {
                last_error_ = "SIGKILL failed";
                return false;
            }
            {
                int wstatus = 0;
                waitpid(n.pid, &wstatus, 0);
            }
            n.alive = false;
            return true;
        case Fault::Pause:
            if (kill(n.pid, SIGSTOP) != 0) {
                last_error_ = "SIGSTOP failed";
                return false;
            }
            return true;
        case Fault::None:
        default:
            last_error_ = "unknown fault";
            return false;
    }
}

bool Harness::clear(int index)
{
    if (index < 0 || index >= static_cast<int>(nodes_.size())) {
        last_error_ = "index out of range";
        return false;
    }
    Node &n = nodes_[static_cast<size_t>(index)];
    if (n.pid <= 0 || !n.alive) {
        last_error_ = "node not running";
        return false;
    }
    if (kill(n.pid, SIGCONT) != 0) {
        last_error_ = "SIGCONT failed";
        return false;
    }
    return true;
}

bool Harness::restart(int index)
{
    if (index < 0 || index >= static_cast<int>(nodes_.size())) {
        last_error_ = "index out of range";
        return false;
    }
    Node &n = nodes_[static_cast<size_t>(index)];
    if (n.pid > 0) {
        kill(n.pid, SIGCONT);
        kill(n.pid, SIGKILL);
        int wstatus = 0;
        waitpid(n.pid, &wstatus, 0);
        n.pid = -1;
    }
    n.alive = false;
    return spawn_node(n);
}

bool Harness::probe(int index, int timeout_ms)
{
    if (index < 0 || index >= static_cast<int>(nodes_.size())) {
        last_error_ = "index out of range";
        return false;
    }
    const Node &n = nodes_[static_cast<size_t>(index)];
    if (!n.alive || n.port == 0) {
        return false;
    }
    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    if (!client) {
        last_error_ = "client create failed";
        return false;
    }
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(n.port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts;
    ts.tv_sec = timeout_ms / 1000;
    ts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
    char info[256] = {0};
    bool connected = vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                        sizeof(addr), &ts, nullptr, info, sizeof(info));
    if (!connected) {
        vsoa_client_close(client);
        return false; /* 故障节点应探测失败 —— 这正是判据 */
    }
    const char *url_str = "/indurtdb/v1/ping";
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(url_str);
    url.url_len = std::strlen(url_str);
    vsoa_payload_t payload;
    std::memset(&payload, 0, sizeof(payload));
    PingResult pr;
    struct timespec cts;
    cts.tv_sec = timeout_ms / 1000;
    cts.tv_nsec = static_cast<long>((timeout_ms % 1000) * 1000000L);
    bool called = vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &payload,
                                   on_ping, &pr, &cts);
    bool ok = false;
    if (called) {
        for (int elapsed = 0; elapsed < timeout_ms && !pr.replied; elapsed += 10) {
            fd_set rfds;
            FD_ZERO(&rfds);
            (void)vsoa_client_fds(client, &rfds);
            struct timeval tv;
            tv.tv_sec = 0;
            tv.tv_usec = 10000;
            int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
            if (r > 0) {
                vsoa_client_input_fds(client, &rfds);
            }
        }
        ok = pr.replied && pr.status == 0x80;
    }
    vsoa_client_close(client);
    if (!ok) {
        last_error_ = "ping not answered with OK";
    }
    return ok;
}

} // namespace vsoa_cluster
