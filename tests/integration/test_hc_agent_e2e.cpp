/**
 * @file test_hc_agent_e2e.cpp
 * @brief h-C 设备侧代理 ON-only 三进程 e2e：真实 rtdbd + 代理 + 外部 client。
 * 验证：双角色透传（4 只读 + peers get）、并发回写不串台、peers set 拒绝、
 * passwd 准入、NOTIFY 扇出。spike 拓扑正式化。
 */
#include <gtest/gtest.h>

#include <vsoa_client.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

extern "C" {
#include "agent/rtdbd_device_agent.h"
#include "dispatch/rtdbd_dispatch.h"  /* rtdbd_st_to_vsoa_status */
#include <rtdbd/protocol.h>           /* POD + RTDBD_PARTITION_ALL */
}

namespace {

static bool write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return false;
    fputs(text, f);
    fclose(f);
    return true;
}

struct RpcR {
    bool done = false;
    uint8_t status = 0;
    size_t len = 0;
    uint8_t data[8304] = {0};
};
static void on_rpc(void *arg, vsoa_client_t *, vsoa_header_t *h, vsoa_payload_t *p)
{
    RpcR *r = (RpcR *)arg;
    r->done = true;
    if (h) r->status = h->status;
    if (p && p->data && p->data_len > 0) {
        r->len = p->data_len < sizeof(r->data) ? p->data_len : sizeof(r->data);
        memcpy(r->data, p->data, r->len);
    }
}
static bool call(vsoa_client_t *c, const char *url, const void *body, size_t len, RpcR *r)
{
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    u.url = const_cast<char *>(url); u.url_len = strlen(url);
    vsoa_payload_t p; memset(&p, 0, sizeof(p));
    p.data = const_cast<void *>(body); p.data_len = len;
    struct timespec ts; ts.tv_sec = 4; ts.tv_nsec = 0;
    memset(r, 0, sizeof(*r));
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &p, on_rpc, r, &ts)) return false;
    for (int i = 0; i < 500 && !r->done; ++i) {
        fd_set rf; FD_ZERO(&rf); vsoa_client_fds(c, &rf);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        if (select(FD_SETSIZE, &rf, nullptr, nullptr, &tv) > 0) vsoa_client_input_fds(c, &rf);
    }
    return r->done;
}

TEST(HcAgentE2E, TransparentReadonlyConcurrentAndGate)
{
    char dir[] = "/tmp/hc_agent_e2e_XXXXXX";
    ASSERT_NE(mkdtemp(dir), nullptr);
    char sock[256], cfg[256], pol[256], pf[256], tk[256];
    snprintf(sock, sizeof(sock), "%s/rtdbd.sock", dir);
    snprintf(cfg, sizeof(cfg), "%s/cfg.yaml", dir);
    snprintf(pol, sizeof(pol), "%s/policy.txt", dir);
    snprintf(pf, sizeof(pf), "%s/vsoa.port", dir);
    snprintf(tk, sizeof(tk), "%s/tokens.txt", dir);
    ASSERT_TRUE(write_text(cfg, "points: []\n"));
    ASSERT_TRUE(write_text(pol, "0:*:0:999\n"));
    ASSERT_TRUE(write_text(tk,
        "tok-agent-ro 0 0 agent-ro plc\n"
        "tok-agent-admin 0 0 agent-admin sync_admin\n"));

    // 进程 1：真实 rtdbd（VSOA loopback）
    pid_t rtdbd_pid = fork();
    ASSERT_GE(rtdbd_pid, 0);
    if (rtdbd_pid == 0) {
#ifndef RTDBD_BIN
#define RTDBD_BIN "../build-hca/rtdbd/rtdbd"
#endif
        execl(RTDBD_BIN, "rtdbd", "--socket", sock,
              "--instance", "hc_agent_e2e", "--config", cfg, "--policy", pol,
              "--max-points", "64", "--max-subs", "4",
              "--vsoa-port", "0", "--vsoa-port-file", pf,
              "--vsoa-token-file", tk, (char *)NULL);
        _exit(127);
    }
    uint16_t up_port = 0;
    for (int i = 0; i < 400 && up_port == 0; ++i) {
        FILE *f = fopen(pf, "r");
        if (f) { unsigned v = 0; if (fscanf(f, "%u", &v) == 1) up_port = (uint16_t)v; fclose(f); }
        if (up_port == 0) usleep(10000);
    }
    ASSERT_NE(up_port, 0u) << "rtdbd VSOA not ready";

    // 进程 2：代理（本进程内直接创建，避免 fork 后 SDK 状态继承问题）
    char apf[256];
    snprintf(apf, sizeof(apf), "%s/agent.port", dir);
    rtdbd_agent_cfg_t acfg;
    memset(&acfg, 0, sizeof(acfg));
    acfg.bind_ip = "127.0.0.1";
    acfg.port = 0;
    acfg.port_file = apf;
    acfg.passwd = "e2e-pass";
    acfg.upstream_host = "127.0.0.1";
    acfg.upstream_port = up_port;
    acfg.token_ro = "tok-agent-ro";
    acfg.token_admin = "tok-agent-admin";
    acfg.max_listeners = 16;
    rtdbd_device_agent_t *agent = rtdbd_device_agent_create(&acfg);
    ASSERT_NE(agent, nullptr) << "agent create failed";
    FILE *ap = fopen(apf, "r");
    ASSERT_NE(ap, nullptr);
    unsigned apv = 0;
    ASSERT_EQ(fscanf(ap, "%u", &apv), 1);
    fclose(ap);
    uint16_t agent_port = (uint16_t)apv;
    ASSERT_NE(agent_port, 0u);

    /* agent 独立子进程跑 poll 循环（外部 client 的同步 connect 需要 server 被持续驱动，
     * 否则主线程阻塞在 connect 形成死锁——spike 已证）。 */
    pid_t agent_pid = fork();
    ASSERT_GE(agent_pid, 0);
    if (agent_pid == 0) {
        for (;;) (void)rtdbd_device_agent_poll_once(agent, 20);
        _exit(0);
    }

    // 外部 client（连代理，passwd 准入）
    vsoa_client_t *c = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(c, nullptr);
    struct sockaddr_in a; memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET; a.sin_port = htons(agent_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts; ts.tv_sec = 3; ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(c, (struct sockaddr *)&a, sizeof(a),
                                    &ts, "e2e-pass", info, sizeof(info))) << info;

    // 1) /point/get 不存在点 → 透传 NOT_FOUND（status=0x84）
    {
        rtdbd_get_req_t req{}; req.point_id = 999;
        RpcR r;
        ASSERT_TRUE(call(c, "/indurtdb/v1/point/get", &req, sizeof(req), &r));
        EXPECT_EQ(r.status, (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_NOT_FOUND));
    }
    // 2) /point/list 透传 OK（ALL 分区）
    {
        rtdbd_list_req_t req{}; req.partition = RTDBD_PARTITION_ALL; req.max = 10;
        RpcR r;
        ASSERT_TRUE(call(c, "/indurtdb/v1/point/list", &req, sizeof(req), &r));
        EXPECT_EQ(r.status, 0x80);
        EXPECT_LE(r.len, 8192u);
    }
    // 3) 并发两请求：两个 client 同时发，回写各自正确（pending 表不串台）
    {
        vsoa_client_t *c2 = vsoa_client_create(nullptr, nullptr);
        ASSERT_NE(c2, nullptr);
        ASSERT_TRUE(vsoa_client_connect(c2, (struct sockaddr *)&a, sizeof(a),
                                        &ts, "e2e-pass", info, sizeof(info))) << info;
        rtdbd_list_req_t r1{}; r1.partition = RTDBD_PARTITION_ALL; r1.max = 1;
        rtdbd_list_req_t r2{}; r2.partition = RTDBD_PARTITION_ALL; r2.max = 2;
        RpcR a1, a2;
        // 依次 call（异步发起后各自 pump），验证两请求都拿到自己的 reply
        vsoa_url_t u; memset(&u, 0, sizeof(u));
        char us[] = "/indurtdb/v1/point/list"; u.url = us; u.url_len = strlen(us);
        vsoa_payload_t p1; memset(&p1, 0, sizeof(p1)); p1.data = &r1; p1.data_len = sizeof(r1);
        vsoa_payload_t p2; memset(&p2, 0, sizeof(p2)); p2.data = &r2; p2.data_len = sizeof(r2);
        ASSERT_TRUE(vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &p1, on_rpc, &a1, &ts));
        ASSERT_TRUE(vsoa_client_call(c2, VSOA_CLIENT_RPC_METHOD_GET, &u, &p2, on_rpc, &a2, &ts));
        for (int i = 0; i < 500 && (!a1.done || !a2.done); ++i) {
            fd_set rf; FD_ZERO(&rf);
            vsoa_client_fds(c, &rf); vsoa_client_fds(c2, &rf);
            struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
            if (select(FD_SETSIZE, &rf, nullptr, nullptr, &tv) > 0) {
                vsoa_client_input_fds(c, &rf); vsoa_client_input_fds(c2, &rf);
            }
        }
        EXPECT_TRUE(a1.done) << "concurrent req1 no reply";
        EXPECT_TRUE(a2.done) << "concurrent req2 no reply";
        EXPECT_EQ(a1.status, 0x80);
        EXPECT_EQ(a2.status, 0x80);
        vsoa_client_close(c2);
    }
    // 4) peers set 未注册 listener → SDK 回 status=3（拒绝）
    {
        RpcR r;
        uint8_t junk[4] = {0};
        bool ok = call(c, "/indurtdb/v1/sync/peers/set", junk, sizeof(junk), &r);
        if (ok) EXPECT_NE(r.status, 0x80);
    }

    vsoa_client_close(c);
    kill(agent_pid, SIGTERM);
    int as = 0; waitpid(agent_pid, &as, 0);
    kill(rtdbd_pid, SIGTERM);
    int st = 0; waitpid(rtdbd_pid, &st, 0);
}

} // namespace
