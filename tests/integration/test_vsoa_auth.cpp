/**
 * @file test_vsoa_auth.cpp
 * @brief v3.12 M3.x / P7 鉴权端到端（ON-only，§11.1 fixture 名 test_vsoa_auth.cpp）。
 *
 * fork 真实 rtdbd（--vsoa-token-file / --vsoa-port-file），用 VSOA client：
 *   1) 错误 token → DENIED（deny_bad_token）；
 *   2) sync_admin 握手成功，但访问镜像能力 /ping 必须 DENIED
 *      （§8.3-4：sync_admin 只做成员管控，不得写/读点位）；
 *   3) sync_admin 的 /sync/peers/set|get 仍 OK（正向，防门禁误伤）；
 *   4) plc 握手后 /ping 仍 OK（正向，防门禁误伤）。
 *
 * 接线前第 2 项会失败（当前 vsoa_on_cmd 的镜像路径无 role→URL 约束）——
 * 这是真实 RED，不是空壳。非 loopback 来源无法在本机建链，其判据由
 * 默认构建的 IR2_AuthDeny 纯逻辑用例以构造地址覆盖。
 */
#include <gtest/gtest.h>

#include <rtdbd/protocol.h>
#include <vsoa_client.h>
#include <vsoa_parser.h>
#include "vsoa/vsoa_adapter.h"

#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
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
        sock = "/tmp/indurtdb_vsoa_auth.sock";
        policy = tmp_file("vsoa_auth_policy.txt",
                          std::to_string((unsigned)getuid()) + ":*:0:999\n");
        config = tmp_file("vsoa_auth_cfg.yaml", "points: []\n");
        tokens = tmp_file("vsoa_auth_tokens.txt",
                          "tok-admin-1 0 0 admin-console sync_admin\n"
                          "tok-plc-1 0 0 plc-line-1 plc\n"
                          "tok-peer-1 0 0 peer-node-1 sync_peer\n");
        port_file = "/tmp/indurtdb_vsoa_auth.port";
        ::unlink(sock.c_str()); ::unlink(port_file.c_str());
        pid = fork();
        if (pid < 0) return false;
        if (pid == 0) {
            execl(RTDBD_BIN, "rtdbd", "--socket", sock.c_str(),
                  "--instance", "vsoa_auth_test", "--policy", policy.c_str(),
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
    ~VsoaCli() { if (c) vsoa_client_close(c); }
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
};

bool hello(VsoaCli &cli, const char *token, const char *service_id, RpcResult &r)
{
    rtdbd_vsoa_hello_req_t h;
    std::memset(&h, 0, sizeof(h));
    std::strcpy(h.token, token);
    std::strcpy(h.service_id, service_id);
    return cli.call("/indurtdb/v1/hello", &h, sizeof(h), r);
}

} // namespace

TEST(VsoaAuth, DenyBadToken)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));
    RpcResult r;
    ASSERT_TRUE(hello(cli, "wrong-token", "admin-console", r));
    EXPECT_EQ(r.status, 0x80 + RTDBD_ST_DENIED) << "错误 token 应被拒（deny_bad_token）";
}

TEST(VsoaAuth, DenySyncAdminMirrorUrl)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));
    RpcResult hr;
    ASSERT_TRUE(hello(cli, "tok-admin-1", "admin-console", hr));
    ASSERT_EQ(hr.status, 0x80) << "sync_admin 握手应先成功";

    RpcResult ping;
    ASSERT_TRUE(cli.call("/indurtdb/v1/ping", nullptr, 0, ping));
    EXPECT_EQ(ping.status, 0x80 + RTDBD_ST_DENIED)
        << "§8.3-4：sync_admin 只做成员管控，不得访问镜像能力 /ping（接线前真实 RED）";
}

TEST(VsoaAuth, AdminSetPeersStillOk)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));
    RpcResult hr;
    ASSERT_TRUE(hello(cli, "tok-admin-1", "admin-console", hr));
    ASSERT_EQ(hr.status, 0x80);

    RpcResult get;
    ASSERT_TRUE(cli.call("/indurtdb/v1/sync/peers/get", nullptr, 0, get));
    EXPECT_EQ(get.status, 0x80) << "正向：sync_admin 的管控 URL 不得被新门禁误伤";
}

TEST(VsoaAuth, PlcMirrorUrlStillOk)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    VsoaCli cli;
    ASSERT_TRUE(cli.connect(proc.port));
    RpcResult hr;
    ASSERT_TRUE(hello(cli, "tok-plc-1", "plc-line-1", hr));
    ASSERT_EQ(hr.status, 0x80);

    RpcResult ping;
    ASSERT_TRUE(cli.call("/indurtdb/v1/ping", nullptr, 0, ping));
    EXPECT_EQ(ping.status, 0x80) << "正向：plc 的镜像能力不得被新门禁误伤";
}

TEST(VsoaAuth, SyncPeerSourceInTable)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    VsoaCli admin;
    ASSERT_TRUE(admin.connect(proc.port));
    RpcResult hr;
    ASSERT_TRUE(hello(admin, "tok-admin-1", "admin-console", hr));
    ASSERT_EQ(hr.status, 0x80) << "admin 握手应先成功";

    /* admin 把 127.0.0.1 注入成员表（sync_peer 客户端来源即 loopback 127.0.0.1）。 */
    rtdbd_peers_resp_t set;
    std::memset(&set, 0, sizeof(set));
    set.count = 1;
    std::snprintf(set.peers[0].addr, sizeof(set.peers[0].addr), "127.0.0.1");
    std::snprintf(set.peers[0].node_id, sizeof(set.peers[0].node_id), "n0");
    set.peers[0].state = RTDBD_PEER_ST_ACTIVE;
    RpcResult setr;
    ASSERT_TRUE(admin.call("/indurtdb/v1/sync/peers/set", &set, sizeof(set), setr));
    ASSERT_EQ(setr.status, 0x80) << "admin 注入成员表应成功";

    /* sync_peer 从 127.0.0.1 握手：来源命中成员表 → 期望 OK。
     * 接线前 vsoa_hello 对 SYNC_PEER 仍返回 NOT_IMPLEMENTED（真实 RED）。 */
    VsoaCli peer;
    ASSERT_TRUE(peer.connect(proc.port));
    RpcResult phr;
    ASSERT_TRUE(hello(peer, "tok-peer-1", "peer-node-1", phr));
    EXPECT_EQ(phr.status, 0x80)
        << "§8.3-4：sync_peer 来源命中成员表应放行（接线前真实 RED）";
}

TEST(VsoaAuth, SyncPeerSourceNotInTableDenied)
{
    ::signal(SIGPIPE, SIG_IGN);
    Proc proc;
    ASSERT_TRUE(proc.start());
    /* 成员表为空时，sync_peer 从 127.0.0.1 握手应被拒（DENIED，非 OK）。
     * 锁定接线：证明 adapter 确实查了成员表，空表 → DENIED，而非恒 OK（防恒绿）。 */
    VsoaCli peer;
    ASSERT_TRUE(peer.connect(proc.port));
    rtdbd_vsoa_hello_req_t h;
    std::memset(&h, 0, sizeof(h));
    std::snprintf(h.token, sizeof(h.token), "tok-peer-1");
    std::snprintf(h.service_id, sizeof(h.service_id), "peer-node-1");
    RpcResult phr;
    ASSERT_TRUE(peer.call("/indurtdb/v1/hello", &h, sizeof(h), phr));
    EXPECT_EQ(phr.status, 0x80 + RTDBD_ST_DENIED)
        << "§8.3-4：sync_peer 来源不在成员表应拒绝（DENIED=2 → 0x82；锁定接线，防恒绿）";
}
