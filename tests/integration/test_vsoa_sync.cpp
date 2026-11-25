/**
 * @file test_vsoa_sync.cpp
 * @brief S2 同步通道两进程 loopback e2e：真 hello + snapshot/delta/wait + 门禁负例。
 * server 子进程 fork（单进程同步 connect 死锁，spike 已证）；server 仍只绑 loopback。
 */
#include <gtest/gtest.h>

extern "C" {
#include "vsoa/vsoa_sync_protocol.h"
#include "cluster/rtdbd_epoch.h"
#include "cluster/rtdbd_digest.h"
#include "cluster/rtdbd_peers_global.h"
#include "dispatch/rtdbd_dispatch.h"
#include <vsoa/vsoa_adapter.h>
}

#include <vsoa_client.h>
#include <rtdbd/protocol.h>
#include <indurtdb/indurtdb.h>

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>

namespace {

struct RpcRes {
    bool done = false;
    uint8_t status = 0;
    char param[256] = {0};
    size_t param_len = 0;
    uint8_t data[8304] = {0};
    size_t data_len = 0;
};

static void on_rpc(void *arg, vsoa_client_t *, vsoa_header_t *h, vsoa_payload_t *p)
{
    RpcRes *r = (RpcRes *)arg;
    r->done = true;
    if (h) r->status = h->status;
    if (p) {
        if (p->param && p->param_len > 0) {
            r->param_len = p->param_len < sizeof(r->param) ? p->param_len : sizeof(r->param);
            memcpy(r->param, p->param, r->param_len);
        }
        if (p->data && p->data_len > 0) {
            r->data_len = p->data_len < sizeof(r->data) ? p->data_len : sizeof(r->data);
            memcpy(r->data, p->data, r->data_len);
        }
    }
}

static bool pump(vsoa_client_t *c, RpcRes *r)
{
    struct timespec end;
    clock_gettime(CLOCK_MONOTONIC, &end);
    end.tv_sec += 5;
    while (!r->done) {
        fd_set rfds; FD_ZERO(&rfds);
        vsoa_client_fds(c, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        if (select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv) > 0)
            vsoa_client_input_fds(c, &rfds);
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end.tv_sec ||
            (now.tv_sec == end.tv_sec && now.tv_nsec > end.tv_nsec)) break;
    }
    return r->done;
}

static bool call_rpc(vsoa_client_t *c, const char *url, const void *req, size_t len,
                     RpcRes *r)
{
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    u.url = const_cast<char *>(url); u.url_len = strlen(url);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = const_cast<void *>(req); pl.data_len = len;
    struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
    memset(r, 0, sizeof(*r));
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl, on_rpc, r, &ts)) return false;
    return pump(c, r);
}

/* server 子进程：真 adapter + sync_peer token + 成员表预置 127.0.0.1。 */
static int sync_noop_handler(void *, uint16_t, const uint8_t *, size_t,
                             uint8_t *, size_t, size_t *)
{
    return RTDBD_ST_NOT_IMPLEMENTED; /* 本测试所有 URL 均为 adapter 专有分支，不走 handler */
}

static void run_sync_server(int wfd, indurtdb_t *h)
{
    rtdbd_vsoa_handler_t handler;
    handler.handle = sync_noop_handler;
    handler.ctx = nullptr;
    rtdbd_vsoa_adapter_t *a = rtdbd_vsoa_adapter_create(&handler);
    if (!a) { _exit(2); }
    rtdbd_dispatch_cfg_t dc; memset(&dc, 0, sizeof(dc));
    rtdbd_vsoa_adapter_set_cfg(a, &dc);
    rtdbd_vsoa_adapter_set_handle(a, h);
    rtdbd_vsoa_adapter_add_token(a, "tok-sync-peer", 0, 0, "peer-node", RTDBD_ROLE_SYNC_PEER);
    rtdbd_vsoa_adapter_add_token(a, "tok-sync-admin", 0, 0, "admin-node", RTDBD_ROLE_SYNC_ADMIN);
    rtdbd_vsoa_adapter_set_sync_node_id(a, "node-local");
    /* 成员表预置对端 127.0.0.1（纯 IP、无端口；self_addr 留空避免自地址拒绝）。 */
    rtdbd_peer_t peers[1];
    memset(peers, 0, sizeof(peers));
    strcpy(peers[0].addr, "127.0.0.1");
    strcpy(peers[0].node_id, "peer-node");
    (void)rtdbd_peers_apply_set(peers, 1);
    uint16_t port = 0;
    if (!rtdbd_vsoa_adapter_start(a, 0, &port)) _exit(3);
    if (write(wfd, &port, sizeof(port)) != (ssize_t)sizeof(port)) _exit(4);
    for (;;) {
        fd_set rfds; FD_ZERO(&rfds);
        rtdbd_vsoa_adapter_fds(a, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        if (select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv) > 0)
            rtdbd_vsoa_adapter_input(a, &rfds);
        rtdbd_vsoa_adapter_wait_pump(a);
    }
}

} // namespace

TEST(VsoaSync, HelloSnapshotDeltaWaitAndGate)
{
    indurtdb_cfg_t cfg{}; cfg.max_points = 64; cfg.max_subscribers = 0;
    char inst[64];
    snprintf(inst, sizeof(inst), "vsoa_sync_%d", (int)getpid());
    indurtdb_t *h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
    for (uint32_t id : {1u, 2u, 5u}) {
        char name[16]; snprintf(name, sizeof(name), "p%u", id);
        ASSERT_EQ(indurtdb_h_create_point(h, id, name, INDURTDB_TYPE_INT32,
                                          INDURTDB_ACCESS_READ_WRITE), 0);
    }
    int pfd[2]; ASSERT_EQ(pipe(pfd), 0);
    pid_t pid = fork(); ASSERT_GE(pid, 0);
    if (pid == 0) { close(pfd[0]); run_sync_server(pfd[1], h); }
    close(pfd[1]);
    uint16_t port = 0;
    ASSERT_EQ(read(pfd[0], &port, sizeof(port)), (ssize_t)sizeof(port));
    close(pfd[0]);
    ASSERT_NE(port, 0);

    vsoa_client_t *c = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(c, nullptr);
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(c, (struct sockaddr *)&addr, sizeof(addr),
                                    &ts, nullptr, info, sizeof(info))) << info;
    /* 真 hello（sync_peer token）建立角色。 */
    rtdbd_vsoa_hello_req_t hr;
    memset(&hr, 0, sizeof(hr));
    strcpy(hr.token, "tok-sync-peer"); strcpy(hr.service_id, "peer-node");
    RpcRes hr_res;
    ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/hello", &hr, sizeof(hr), &hr_res));
    ASSERT_EQ(hr_res.status, 0x80);

    // 1) /sync/hello：交换 epoch/digest/node_id
    {
        rtdbd_sync_hello_req_t rq{};
        rq.epoch = 11; rq.struct_hash = 0xAA; rq.value_digest = 0xBB;
        rq.n_points = 3; strcpy(rq.node_id, "peer-node");
        RpcRes r;
        ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/sync/hello", &rq, sizeof(rq), &r));
        EXPECT_EQ(r.status, 0x80);
        ASSERT_EQ(r.data_len, sizeof(rtdbd_sync_hello_resp_t));
        rtdbd_sync_hello_resp_t rp; memcpy(&rp, r.data, sizeof(rp));
        EXPECT_EQ(rp.accepted, 1u);
        EXPECT_STREQ(rp.node_id, "node-local");
        EXPECT_EQ(rp.n_points, 3u);
    }
    // 2) /sync/snapshot：header(param)+点位(data) 分页
    {
        rtdbd_sync_snapshot_req_t rq{}; rq.page_index = 0; rq.page_size = 2;
        RpcRes r;
        ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/sync/snapshot", &rq, sizeof(rq), &r));
        EXPECT_EQ(r.status, 0x80);
        ASSERT_GE(r.param_len, sizeof(rtdbd_sync_snapshot_resp_t));
        rtdbd_sync_snapshot_resp_t hp; memcpy(&hp, r.param, sizeof(hp));
        EXPECT_EQ(hp.page_index, 0u);
        EXPECT_EQ(hp.total_pages, 2u); // 3 点 / 2 per page
        EXPECT_EQ(hp.n_points, 2u);
        EXPECT_EQ(r.data_len, 2u * sizeof(indurtdb_point_t));
    }
    // 3) /sync/delta：两次游标不重不漏
    {
        rtdbd_sync_delta_req_t rq{}; rq.from_offset = 0; rq.max_records = 2;
        RpcRes r1;
        ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/sync/delta", &rq, sizeof(rq), &r1));
        EXPECT_EQ(r1.status, 0x80);
        ASSERT_GE(r1.param_len, sizeof(rtdbd_sync_delta_resp_t));
        rtdbd_sync_delta_resp_t d1; memcpy(&d1, r1.param, sizeof(d1));
        EXPECT_EQ(d1.n_records, 2u);
        EXPECT_EQ(d1.has_more, 1u);
        rq.from_offset = d1.applied_offset;
        RpcRes r2;
        ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/sync/delta", &rq, sizeof(rq), &r2));
        rtdbd_sync_delta_resp_t d2; memcpy(&d2, r2.param, sizeof(d2));
        EXPECT_EQ(d2.n_records, 1u);
        EXPECT_EQ(d2.has_more, 0u);
    }
    // 4) /sync/wait：required_acks=1 成功（peer 已 hello epoch 11）；>1 超时返回实际 acks
    {
        rtdbd_sync_wait_req_t rq{};
        rq.min_epoch = 1; rq.required_acks = 1; rq.timeout_ms = 3000;
        RpcRes r;
        ASSERT_TRUE(call_rpc(c, "/indurtdb/v1/sync/wait", &rq, sizeof(rq), &r));
        EXPECT_EQ(r.status, 0x80);
        ASSERT_EQ(r.data_len, sizeof(rtdbd_sync_wait_resp_t));
        rtdbd_sync_wait_resp_t wp; memcpy(&wp, r.data, sizeof(wp));
        EXPECT_EQ(wp.acks, 1u);
        EXPECT_EQ(wp.ok, 1u);
    }
    // 5) 门禁负例：sync_admin 对 /sync/* DENIED
    {
        vsoa_client_t *admin = vsoa_client_create(nullptr, nullptr);
        ASSERT_NE(admin, nullptr);
        ASSERT_TRUE(vsoa_client_connect(admin, (struct sockaddr *)&addr, sizeof(addr),
                                        &ts, nullptr, info, sizeof(info)));
        rtdbd_vsoa_hello_req_t ah;
        memset(&ah, 0, sizeof(ah));
        strcpy(ah.token, "tok-sync-admin"); strcpy(ah.service_id, "admin-node");
        RpcRes ar;
        ASSERT_TRUE(call_rpc(admin, "/indurtdb/v1/hello", &ah, sizeof(ah), &ar));
        ASSERT_EQ(ar.status, 0x80);
        rtdbd_sync_hello_req_t rq{}; rq.epoch = 1;
        RpcRes r;
        ASSERT_TRUE(call_rpc(admin, "/indurtdb/v1/sync/hello", &rq, sizeof(rq), &r));
        EXPECT_EQ(r.status, (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_DENIED));
        vsoa_client_close(admin);
    }

    vsoa_client_close(c);
    kill(pid, SIGTERM);
    int st = 0; waitpid(pid, &st, 0);
    indurtdb_h_close(h);
}
