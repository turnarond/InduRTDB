/**
 * @file test_vsoa_peers_admin.cpp
 * @brief v3.12 M3.3 切片 C：VSOA /sync/peers/{set,get} + role=sync_admin（ON-only）。
 *
 * fork 真实 rtdbd（--vsoa-token-file / --vsoa-port-file），用 VSOA client：
 *   1) /hello 以 sync_admin token 握手；
 *   2) /sync/peers/set 整组替换 → /sync/peers/get 读回；
 *   3) UDS OP_GET_PEERS 必须读到同一张表且 state=JOINING
 *      （证明共享 apply + 延迟动作队列跨前端在循环尾执行）；
 *   4) 未握手 / role=plc 一律 DENIED。
 * 未接线时 rtdbd 因未知参数退出 → RED。
 */
#include <gtest/gtest.h>

#include <rtdbd/protocol.h>
#include <vsoa_client.h>
#include <vsoa_parser.h>
#include "vsoa/vsoa_adapter.h"

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
#include <string>

#ifndef RTDBD_BIN
#define RTDBD_BIN ""
#endif

namespace {
std::string tmp_file(const std::string &n, const std::string &body)
{
    std::string p = "/tmp/" + n;
    FILE *f = std::fopen(p.c_str(), "w");
    if (f) { std::fwrite(body.data(), 1, body.size(), f); std::fclose(f); }
    return p;
}

struct Proc {
    std::string sock, policy, config, port_file, tokens;
    uint16_t port = 0;
    pid_t pid = -1;
    ~Proc() { stop(); }
    bool start()
    {
        sock = "/tmp/indurtdb_vsoa_admin.sock";
        policy = tmp_file("vsoa_admin_policy.txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("vsoa_admin_cfg.yaml", "points: []\n");
        tokens = tmp_file("vsoa_admin_tokens.txt",
                          "tok-admin-1 0 0 admin-console sync_admin\n"
                          "tok-plc-1 0 0 plc-line-1 plc\n");
        port_file = "/tmp/indurtdb_vsoa_admin.port";
        ::unlink(sock.c_str()); ::unlink(port_file.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd", "--socket", sock.c_str(),
                  "--instance", "vsoa_admin_test", "--policy", policy.c_str(),
                  "--config", config.c_str(), "--max-points", "64", "--max-subs", "4",
                  "--vsoa-port", "0", "--vsoa-port-file", port_file.c_str(),
                  "--vsoa-token-file", tokens.c_str(), (char *)nullptr);
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
        if (pid > 0) { kill(pid, SIGTERM); int st = 0; waitpid(pid, &st, 0); pid = -1; }
        ::unlink(sock.c_str()); ::unlink(port_file.c_str());
    }
};

struct RpcResult {
    bool replied = false;
    uint8_t status = 0;
    size_t data_len = 0;
    uint8_t data[8192];
};
void on_rpc(void *arg, vsoa_client_t *, vsoa_header_t *h, vsoa_payload_t *p)
{
    RpcResult *r = static_cast<RpcResult *>(arg);
    r->replied = true;
    if (h) r->status = (uint8_t)h->status;
    if (p && p->data && p->data_len > 0) {
        r->data_len = p->data_len < sizeof(r->data) ? p->data_len : sizeof(r->data);
        std::memcpy(r->data, p->data, r->data_len);
    }
}

struct VsoaCli {
    vsoa_client_t *c = nullptr;
    bool connect(uint16_t port)
    {
        c = vsoa_client_create(nullptr, nullptr);
        if (!c) return false;
        struct sockaddr_in a;
        std::memset(&a, 0, sizeof(a));
        a.sin_family = AF_INET; a.sin_port = htons(port);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        struct timespec ts{2, 0};
        char info[256] = {0};
        return vsoa_client_connect(c, reinterpret_cast<struct sockaddr *>(&a),
                                   sizeof(a), &ts, nullptr, info, sizeof(info));
    }
    bool call(const char *url_str, const void *req, size_t req_len, RpcResult &r)
    {
        vsoa_url_t u; std::memset(&u, 0, sizeof(u));
        u.url = const_cast<char *>(url_str); u.url_len = std::strlen(url_str);
        vsoa_payload_t pl; std::memset(&pl, 0, sizeof(pl));
        if (req && req_len) {
            pl.data = const_cast<char *>(static_cast<const char *>(req));
            pl.data_len = req_len;
        }
        struct timespec ts{3, 0};
        if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl, on_rpc, &r, &ts)) return false;
        for (int e = 0; e < 2000 && !r.replied; e += 10) {
            fd_set rfds; FD_ZERO(&rfds);
            (void)vsoa_client_fds(c, &rfds);
            struct timeval tv{0, 10000};
            if (select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv) > 0) vsoa_client_input_fds(c, &rfds);
        }
        return r.replied;
    }
    ~VsoaCli() { if (c) vsoa_client_close(c); }
};

bool uds_get_peers(const std::string &sock, rtdbd_peers_resp_t &out)
{
    int fd = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return false;
    struct sockaddr_un a; std::memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    size_t n = sock.size();
    if (n >= sizeof(a.sun_path)) { ::close(fd); return false; }
    std::memcpy(a.sun_path, sock.c_str(), n + 1);
    bool ok = false;
    if (::connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        rtdbd_req_hdr_t q; std::memset(&q, 0, sizeof(q));
        q.magic = RTDBD_MAGIC; q.version = RTDBD_PROTO_VERSION;
        q.opcode = RTDBD_OP_GET_PEERS; q.payload_len = 0;
        rtdbd_resp_hdr_t h; std::memset(&h, 0, sizeof(h));
        if (::write(fd, &q, sizeof(q)) == (ssize_t)sizeof(q) &&
            ::read(fd, &h, sizeof(h)) == (ssize_t)sizeof(h) &&
            h.status == RTDBD_ST_OK && h.payload_len == sizeof(out) &&
            ::read(fd, &out, sizeof(out)) == (ssize_t)sizeof(out)) ok = true;
    }
    ::close(fd);
    return ok;
}

void fill_set(rtdbd_peers_resp_t &s)
{
    std::memset(&s, 0, sizeof(s));
    s.count = 2;
    const char *addrs[] = {"10.50.0.1", "10.50.0.2"};
    for (uint32_t i = 0; i < s.count; ++i) {
        std::snprintf(s.peers[i].addr, sizeof(s.peers[i].addr), "%s", addrs[i]);
        std::snprintf(s.peers[i].node_id, sizeof(s.peers[i].node_id), "n%u", i);
        s.peers[i].state = RTDBD_PEER_ST_ACTIVE;
    }
}

}  // namespace

TEST(VsoaPeersAdmin, HelloSetGetAndUdsSharesSameTable)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start()) << "rtdbd 双栈 + token file 未就绪（参数未接线？）";
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));

    rtdbd_vsoa_hello_req_t hello;
    std::memset(&hello, 0, sizeof(hello));
    std::strcpy(hello.token, "tok-admin-1");
    std::strcpy(hello.service_id, "admin-console");
    RpcResult r;
    ASSERT_TRUE(cli.call("/indurtdb/v1/hello", &hello, sizeof(hello), r));
    ASSERT_EQ(r.status, 0x80) << "sync_admin 握手被拒";

    static rtdbd_peers_resp_t set;
    fill_set(set);
    RpcResult r2;
    ASSERT_TRUE(cli.call("/indurtdb/v1/sync/peers/set", &set, sizeof(set), r2));
    EXPECT_EQ(r2.status, 0x80) << "管控 set 失败";

    RpcResult r3;
    ASSERT_TRUE(cli.call("/indurtdb/v1/sync/peers/get", nullptr, 0, r3));
    ASSERT_EQ(r3.status, 0x80);
    ASSERT_EQ(r3.data_len, sizeof(rtdbd_peers_resp_t));
    rtdbd_peers_resp_t got;
    std::memcpy(&got, r3.data, sizeof(got));
    ASSERT_EQ(got.count, 2u);
    EXPECT_STREQ(got.peers[0].addr, "10.50.0.1");

    /* 关键：等主循环循环尾 drain 执行（poll 周期 500ms），延迟动作把
     * JOINING 推进为 ACTIVE —— 验证共享 apply + 延迟动作闭环 + 状态机。 */
    usleep(1500000);
    rtdbd_peers_resp_t uds;
    ASSERT_TRUE(uds_get_peers(proc.sock, uds));
    ASSERT_EQ(uds.count, 2u);
    EXPECT_STREQ(uds.peers[0].addr, "10.50.0.1");
    EXPECT_EQ(uds.peers[0].state, RTDBD_PEER_ST_ACTIVE)
        << "跨前端共享 apply / 循环尾延迟动作 JOINING->ACTIVE 未闭环";
}

TEST(VsoaPeersAdmin, DeniesUnauthenticatedAndPlcRole)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());

    VsoaCli guest;
    ASSERT_TRUE(guest.connect(proc.port));
    RpcResult r;
    ASSERT_TRUE(guest.call("/indurtdb/v1/sync/peers/get", nullptr, 0, r));
    EXPECT_EQ(r.status, 0x80 + RTDBD_ST_DENIED) << "未握手应被拒";

    VsoaCli plc;
    ASSERT_TRUE(plc.connect(proc.port));
    rtdbd_vsoa_hello_req_t h;
    std::memset(&h, 0, sizeof(h));
    std::strcpy(h.token, "tok-plc-1");
    std::strcpy(h.service_id, "plc-line-1");
    RpcResult hr;
    ASSERT_TRUE(plc.call("/indurtdb/v1/hello", &h, sizeof(h), hr));
    ASSERT_EQ(hr.status, 0x80);
    RpcResult r2;
    ASSERT_TRUE(plc.call("/indurtdb/v1/sync/peers/get", nullptr, 0, r2));
    EXPECT_EQ(r2.status, 0x80 + RTDBD_ST_DENIED) << "role=plc 不得访问成员管控";
}

TEST(VsoaPeersAdmin, IR1_JoinStateMachineJoiningToActive)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start()) << "rtdbd 双栈 + token file 未就绪";
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));

    rtdbd_vsoa_hello_req_t hello;
    std::memset(&hello, 0, sizeof(hello));
    std::strcpy(hello.token, "tok-admin-1");
    std::strcpy(hello.service_id, "admin-console");
    RpcResult r;
    ASSERT_TRUE(cli.call("/indurtdb/v1/hello", &hello, sizeof(hello), r));
    ASSERT_EQ(r.status, 0x80);

    static rtdbd_peers_resp_t set;
    std::memset(&set, 0, sizeof(set));
    set.count = 3;
    const char *addrs[] = {"10.60.0.1", "10.60.0.2", "10.60.0.3"};
    for (uint32_t i = 0; i < set.count; ++i) {
        std::snprintf(set.peers[i].addr, sizeof(set.peers[i].addr), "%s", addrs[i]);
        std::snprintf(set.peers[i].node_id, sizeof(set.peers[i].node_id), "n%u", i);
        set.peers[i].state = RTDBD_PEER_ST_ACTIVE;
    }
    RpcResult r2;
    ASSERT_TRUE(cli.call("/indurtdb/v1/sync/peers/set", &set, sizeof(set), r2));
    EXPECT_EQ(r2.status, 0x80);

    /* 延迟等待循环尾 drain：PEER_ADD(JOINING) -> TRIGGER_FULLSYNC(ACTIVE)。
     * 覆盖 IR1「成员加入状态机」：入队不假装完成，执行器在循环尾落地。 */
    usleep(1600000);
    rtdbd_peers_resp_t uds;
    ASSERT_TRUE(uds_get_peers(proc.sock, uds));
    ASSERT_EQ(uds.count, 3u);
    for (uint32_t i = 0; i < uds.count; ++i) {
        EXPECT_EQ(uds.peers[i].state, RTDBD_PEER_ST_ACTIVE)
            << "IR1 状态机 JOINING->ACTIVE 未闭环 @ " << i;
    }
}
