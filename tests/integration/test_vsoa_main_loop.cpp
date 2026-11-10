/**
 * @file test_vsoa_main_loop.cpp
 * @brief v3.12 M3.3 方案 A：单线程统一 poll 主循环的端到端判据（ON-only）。
 *
 * fork **真实 rtdbd 二进制**（不是测试内自建 adapter），带 --vsoa-port 0
 * + --vsoa-port-file；交替打 UDS PING/GET_PEERS 与 VSOA /ping，验证：
 *   1) 两栈在同一进程、同一事件循环中同时可达；
 *   2) VSOA 连续请求后 UDS 控制回路仍响应（不饿死）。
 * 未接线时 rtdbd 因未知参数退出 → port file 不出现 → RED。
 */
#include <gtest/gtest.h>

#include <rtdbd/protocol.h>
#include <vsoa_client.h>
#include <vsoa_parser.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {

std::string tmp_file(const std::string &name, const std::string &body)
{
    std::string p = "/tmp/" + name;
    FILE *f = std::fopen(p.c_str(), "w");
    if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
    return p;
}

struct Proc {
    std::string sock, policy, config, port_file;
    uint16_t port = 0;
    pid_t pid = -1;
    ~Proc() { stop(); }
    bool start()
    {
        sock = "/tmp/indurtdb_vsoa_main.sock";
        policy = tmp_file("vsoa_main_policy.txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("vsoa_main_cfg.yaml", "points: []\n");
        port_file = "/tmp/indurtdb_vsoa_main.port";
        ::unlink(sock.c_str());
        ::unlink(port_file.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd", "--socket", sock.c_str(),
                  "--instance", "vsoa_main_test", "--policy", policy.c_str(),
                  "--config", config.c_str(), "--max-points", "64", "--max-subs", "4",
                  "--vsoa-port", "0", "--vsoa-port-file", port_file.c_str(),
                  (char *)nullptr);
            _exit(127);
        }
        for (int i = 0; i < 200; ++i) {
            struct stat st;
            if (::stat(sock.c_str(), &st) == 0) break;
            usleep(10000);
        }
        for (int i = 0; i < 200; ++i) {
            FILE *f = std::fopen(port_file.c_str(), "r");
            if (f) {
                unsigned v = 0;
                if (std::fscanf(f, "%u", &v) == 1) port = (uint16_t)v;
                std::fclose(f);
                if (port != 0) return true;
            }
            usleep(10000);
        }
        return false;
    }
    void stop()
    {
        if (pid > 0) { kill(pid, SIGTERM); int s = 0; waitpid(pid, &s, 0); pid = -1; }
        ::unlink(sock.c_str());
        ::unlink(port_file.c_str());
    }
};

/* ---- UDS 侧：原始 12B 头请求 / 12B 头响应 ---- */
bool uds_ping(const std::string &sock)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un a;
    std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    std::strncpy(a.sun_path, sock.c_str(), sizeof(a.sun_path) - 1);
    bool ok = false;
    if (::connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        rtdbd_req_hdr_t req;
        std::memset(&req, 0, sizeof(req));
        req.magic = RTDBD_MAGIC; req.version = RTDBD_PROTO_VERSION;
        req.opcode = RTDBD_OP_PING; req.payload_len = 0;
        rtdbd_resp_hdr_t resp;
        std::memset(&resp, 0, sizeof(resp));
        if (::write(fd, &req, sizeof(req)) == (ssize_t)sizeof(req) &&
            ::read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp) &&
            resp.status == RTDBD_ST_OK) {
            ok = true;
        }
    }
    ::close(fd);
    return ok;
}

/* ---- VSOA 侧：client RPC + 事件泵 ---- */
struct RpcResult { bool replied = false; uint8_t status = 0; };

void on_rpc(void *arg, vsoa_client_t *, vsoa_header_t *hdr, vsoa_payload_t *)
{
    RpcResult *r = static_cast<RpcResult *>(arg);
    r->replied = true;
    if (hdr) r->status = hdr->status;
}

bool pump(vsoa_client_t *c, RpcResult *r, int wait_ms)
{
    for (int e = 0; e < wait_ms && !r->replied; e += 10) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)vsoa_client_fds(c, &rfds);
        struct timeval tv{0, 10000};
        if (select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv) > 0) {
            vsoa_client_input_fds(c, &rfds);
        }
    }
    return r->replied;
}

bool vsoa_ping(uint16_t port)
{
    vsoa_client_t *c = vsoa_client_create(nullptr, nullptr);
    if (!c) return false;
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts{2, 0};
    char info[256] = {0};
    bool ok = false;
    if (vsoa_client_connect(c, reinterpret_cast<struct sockaddr *>(&addr),
                            sizeof(addr), &ts, nullptr, info, sizeof(info))) {
        const char *url_str = "/indurtdb/v1/ping";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(url_str);
        url.url_len = std::strlen(url_str);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        RpcResult r;
        struct timespec call_ts{3, 0};
        if (vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                             on_rpc, &r, &call_ts) &&
            pump(c, &r, 2000) && r.status == 0x80) {
            ok = true;
        }
    }
    vsoa_client_close(c);
    return ok;
}

}  // namespace

TEST(VsoaMainLoop, BothStacksRespondInSameProcess)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start()) << "rtdbd 未同时就绪 UDS socket 与 VSOA 端口文件"
                                 "（--vsoa-port/--vsoa-port-file 未接线？）";
    ASSERT_NE(proc.port, 0u);
    EXPECT_TRUE(uds_ping(proc.sock)) << "UDS 控制回路无响应";
    EXPECT_TRUE(vsoa_ping(proc.port)) << "VSOA 服务无响应（adapter 未接进 main？）";
    EXPECT_TRUE(uds_ping(proc.sock)) << "VSOA 请求后 UDS 不再响应";
}

TEST(VsoaMainLoop, VsoaTrafficDoesNotStarveUds)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    for (int i = 0; i < 5; ++i) {
        ASSERT_TRUE(vsoa_ping(proc.port)) << "第 " << i << " 轮 VSOA 失败";
        ASSERT_TRUE(uds_ping(proc.sock)) << "第 " << i << " 轮 UDS 被饿死";
    }
}
