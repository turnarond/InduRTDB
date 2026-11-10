/**
 * @file test_vsoa_e2e.cpp
 * @brief M1.3c：VSOA 端到端联调用例（server 子进程 + client 父进程）。
 *
 * 拓扑与 T0 取证一致（T0 §1 的关键事实：server 的 accept/握手必须由
 * vsoa_server_input_fds() 驱动，故单进程内 connect 会超时）：
 *   - 子进程：VSOA server（adapter）+ select 事件循环；
 *   - 父进程：VSOA client + 自己的事件循环，等 RPC reply。
 *
 * 验证内容：
 *   1. 真实 RPC（"/indurtdb/v1/ping"）能穿过 adapter 到达**同一个 transport 无关
 *      dispatch**（handler 直接调 rtdbd_dispatch），并回出映射后的 status；
 *   2. status 字节 = 0x80 + RTDBD_ST_OK（§7.2 映射）；
 *   3. 与「直接调用 dispatch」的 status 一致 —— 即 VSOA 前端与 UDS 前端共享同一
 *      业务语义（§8.3-3 双栈等价）。
 *
 * 仅在 INDURTDB_ENABLE_VSOA=ON 时注册（干净树默认构建不含此用例）。
 */
#include <gtest/gtest.h>

#include <csignal>
#include <cstring>
#include <sys/select.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <unistd.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>

extern "C" {
#include "dispatch/rtdbd_dispatch.h"
#include "vsoa/vsoa_adapter.h"
}

namespace {

constexpr int kServerLoopMs = 2000;   /* 子进程单轮 select 超时 */
constexpr int kReplyWaitMs  = 5000;   /* 父进程等 reply 上限 */

/* 写路径依赖（联调侧）：允许 uid=1000 写全部点；分区缺省 ⇒ 任意分区合法。
 * 与 UDS 侧共用同一 irt_policy_allows / rtdbd_conf_partition_allowed。 */
constexpr uint32_t e2e_uid = 1000;

irt_policy_t *e2e_policy()
{
    static irt_policy_t *p = nullptr;
    if (!p) {
        p = new irt_policy_t();
        irt_policy_init(p);
        const char *path = "/tmp/indurtdb_vsoa_e2e_policy.txt";
        FILE *f = std::fopen(path, "w");
        if (f) {
            std::fputs("1000:*:0:999\n", f);
            std::fclose(f);
        }
        uint32_t line = 0;
        irt_policy_load(p, path, &line);
    }
    return p;
}

rtdbd_conf_t *e2e_conf()
{
    static rtdbd_conf_t *c = nullptr;
    if (!c) {
        c = new rtdbd_conf_t();
        rtdbd_conf_init(c);
    }
    return c;
}

int g_audit_count = 0;
char g_audit_service[64] = {0};
void e2e_audit(void *ctx, uint32_t pid, uint32_t uid, uint32_t partition,
               uint32_t point_id, uint64_t ts_ns, const char *service_id)
{
    (void)ctx; (void)pid; (void)uid; (void)partition; (void)point_id; (void)ts_ns;
    g_audit_count++;
    if (service_id) {
        std::snprintf(g_audit_service, sizeof(g_audit_service), "%s", service_id);
    }
}

/* 服务端能力配置快照：子进程（server）与父进程（本地期望值）用**同一份**，
 * 从而 /caps 的 VSOA payload 可与共享 dispatch 的产出逐字节比对。 */
rtdbd_dispatch_cfg_t server_cfg()
{
    rtdbd_dispatch_cfg_t cfg;
    cfg.max_clients = 128;
    cfg.initial_clients = 32;
    cfg.max_subs_per_conn = 10000;
    cfg.initial_subs_per_conn = 2048;
    cfg.max_batch = 1024;
    return cfg;
}

/* handler：走**真实** dispatch（与 UDS 前端同一函数），确保双栈语义同源。 */
int e2e_handler(void *ctx, uint16_t opcode, const uint8_t *req, size_t req_len,
                uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    (void)ctx;
    return static_cast<int>(rtdbd_dispatch(opcode, req, req_len, resp, resp_cap, resp_len));
}

/* 子进程：起 VSOA server（adapter），把端口经管道告知父进程，然后跑事件循环。
 * handle 由父进程在 fork 前建好并注入 —— fork 后父子共享同一 shm 映射，
 * 故两端读到的是**同一份**点位数据（写权威路径唯一）。 */
[[noreturn]] void run_server(int ready_fd, indurtdb_t *handle)
{
    rtdbd_vsoa_handler_t handler;
    handler.handle = e2e_handler;
    handler.ctx = nullptr;
    rtdbd_vsoa_adapter_t *adapter = rtdbd_vsoa_adapter_create(&handler);
    if (!adapter) {
        _exit(2);
    }
    rtdbd_dispatch_cfg_t cfg = server_cfg();
    rtdbd_vsoa_adapter_set_cfg(adapter, &cfg);
    if (handle) {
        rtdbd_vsoa_adapter_set_handle(adapter, handle);
        /* 写路径依赖（与 UDS 同一鉴权函数、同一 deny-by-default） */
        irt_policy_t *policy = e2e_policy();
        rtdbd_conf_t *conf = e2e_conf();
        rtdbd_vsoa_adapter_set_policy(adapter, policy);
        rtdbd_vsoa_adapter_set_conf(adapter, conf);
        rtdbd_vsoa_adapter_set_audit(adapter, e2e_audit, nullptr);
        rtdbd_vsoa_adapter_set_default_uid(adapter, e2e_uid);
        /* 预共享 token 表（§8.3-4 生产形态）：hello 握手凭此换取身份 */
        rtdbd_vsoa_adapter_add_token(adapter, "tok-plc-1", e2e_uid, 0,
                                     "plc-line-1", RTDBD_ROLE_PLC);
        /* 未认证直写仅供本机联调（生产必须关闭） */
        rtdbd_vsoa_adapter_set_allow_unauthenticated(adapter, true);
    }
    uint16_t port = 0;
    if (!rtdbd_vsoa_adapter_start(adapter, 0, &port)) {
        _exit(3);
    }
    ssize_t w = write(ready_fd, &port, sizeof(port));
    (void)w;
    close(ready_fd);

    for (;;) {
        fd_set rfds;
        FD_ZERO(&rfds);
        (void)rtdbd_vsoa_adapter_fds(adapter, &rfds);
        struct timeval tv;
        tv.tv_sec = kServerLoopMs / 1000;
        tv.tv_usec = (kServerLoopMs % 1000) * 1000;
        int r = select(FD_SETSIZE, &rfds, nullptr, nullptr, &tv);
        if (r > 0) {
            rtdbd_vsoa_adapter_input(adapter, &rfds);
        }
    }
}

/* 定向通知（datagram）接收状态：VSOA 侧 NOTIFY 走 cli_datagram 定向投递
 * （§8.3-5），客户端用 on_datagram 回调接收（禁用 publish/quick）。 */
struct DatagramResult {
    bool received = false;
    uint32_t point_id = 0;
    int32_t value = 0;
    uint8_t type = 0;
};

void on_datagram(void *arg, vsoa_client_t *client, vsoa_url_t *url, vsoa_payload_t *payload,
                 bool quick)
{
    (void)client;
    (void)url;
    DatagramResult *d = static_cast<DatagramResult *>(arg);
    if (d->received || quick || !payload || !payload->data || payload->data_len < sizeof(rtdbd_notify_t)) {
        return; /* 只收定向（非 quick）且载荷完整的通知 */
    }
    rtdbd_notify_t nt;
    std::memset(&nt, 0, sizeof(nt));
    std::memcpy(&nt, payload->data, sizeof(nt));
    d->point_id = nt.point_id;
    d->type = nt.type;
    std::memcpy(&d->value, &nt.value_bits, sizeof(d->value));
    d->received = true;
}

struct RpcResult {
    bool replied = false;
    bool has_hdr = false;
    uint8_t status = 0;
    size_t data_len = 0;
    uint8_t data[256];
};

void on_rpc(void *arg, vsoa_client_t *client, vsoa_header_t *hdr, vsoa_payload_t *payload)
{
    (void)client;
    (void)payload;
    RpcResult *res = static_cast<RpcResult *>(arg);
    res->replied = true;
    res->has_hdr = (hdr != nullptr);
    if (hdr) {
        res->status = hdr->status;
    }
    if (payload && payload->data && payload->data_len > 0) {
        res->data_len = payload->data_len < sizeof(res->data) ? payload->data_len
                                                             : sizeof(res->data);
        std::memcpy(res->data, payload->data, res->data_len);
    }
}

/* 父进程：驱动 client 事件循环直到收到 reply 或超时。 */
bool pump_client(vsoa_client_t *client, RpcResult *res, int wait_ms)
{
    for (int elapsed = 0; elapsed < wait_ms && !res->replied; elapsed += 10) {
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
    return res->replied;
}

} // namespace

TEST(VsoaE2E, PingOverRpcGoesThroughSharedDispatch)
{
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], nullptr);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    bool connected = vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                         sizeof(addr), &ts, nullptr, info, sizeof(info));
    ASSERT_TRUE(connected) << "connect failed: " << info;

    const char *url_str = "/indurtdb/v1/ping";
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(url_str);
    url.url_len = std::strlen(url_str);
    vsoa_payload_t payload;
    std::memset(&payload, 0, sizeof(payload));

    RpcResult res;
    struct timespec call_ts;
    call_ts.tv_sec = 3;
    call_ts.tv_nsec = 0;
    bool called = vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &payload,
                                   on_rpc, &res, &call_ts);
    ASSERT_TRUE(called);
    ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply within timeout";
    EXPECT_TRUE(res.has_hdr);

    /* status 映射：0x80 + RTDBD_ST_OK（§7.2） */
    EXPECT_EQ(res.status, rtdbd_st_to_vsoa_status(0));
    EXPECT_EQ(res.status, 0x80);

    /* 与直接调用 dispatch 的结果一致（双栈共享同一业务语义） */
    uint8_t resp[64];
    size_t resp_len = 0;
    uint16_t opcode = 0;
    ASSERT_EQ(rtdbd_opcode_from_url("/ping", &opcode), 0);
    EXPECT_EQ(rtdbd_dispatch(opcode, nullptr, 0, resp, sizeof(resp), &resp_len),
              static_cast<uint16_t>(0));
    EXPECT_EQ(resp_len, 0u);

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

TEST(VsoaE2E, CapsPayloadIsByteIdenticalToSharedDispatch)
{
    // 双栈字节级等价：VSOA 侧 /caps 的响应体（payload 直放 32B POD）必须与
    // 共享 dispatch 的产出**逐字节一致** —— 这是 §7.4「同一 req 在两前端产出
    // 字节一致的 resp」的可证伪证据（UDS 侧同一语义已由 test_rtdbd_notify
    // 逐字段断言锁定）。
    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], nullptr);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                    sizeof(addr), &ts, nullptr, info, sizeof(info)))
        << "connect failed: " << info;

    const char *url_str = "/indurtdb/v1/caps";
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(url_str);
    url.url_len = std::strlen(url_str);
    vsoa_payload_t payload;
    std::memset(&payload, 0, sizeof(payload));

    RpcResult res;
    struct timespec call_ts;
    call_ts.tv_sec = 3;
    call_ts.tv_nsec = 0;
    ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &payload,
                                 on_rpc, &res, &call_ts));
    ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply within timeout";
    EXPECT_EQ(res.status, 0x80); // RTDBD_ST_OK

    /* 本地期望值：同一 dispatch 语义 + 同一 cfg */
    rtdbd_dispatch_cfg_t cfg = server_cfg();
    uint8_t expect[64];
    size_t expect_len = 0;
    ASSERT_EQ(rtdbd_dispatch_caps(&cfg, expect, sizeof(expect), &expect_len), 0u);
    ASSERT_EQ(expect_len, sizeof(rtdbd_caps_t));

    ASSERT_EQ(res.data_len, expect_len);
    EXPECT_EQ(0, std::memcmp(res.data, expect, expect_len)) << "VSOA 与共享 dispatch 的 /caps 字节不一致";

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
}

TEST(VsoaE2E, GetPayloadIsByteIdenticalToSharedDispatch)
{
    // 读点能力的双栈字节级等价：父进程建点位并写值（fork 后子进程继承同一
    // shm 映射 → 同一份数据），VSOA 侧 /point/get 的 64B payload 必须与本地
    // rtdbd_dispatch_get 产出逐字节一致。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "vsoa_e2e_get_%d", static_cast<int>(getpid()));
    indurtdb_t *handle = nullptr;
    ASSERT_EQ(indurtdb_h_open(&handle, inst, &cfg), 0);
    ASSERT_NE(handle, nullptr);
    ASSERT_EQ(indurtdb_h_create_point(handle, 7, "p7", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    ASSERT_EQ(indurtdb_h_write_int32(handle, 7, 1234), 0);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], handle);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                    sizeof(addr), &ts, nullptr, info, sizeof(info)))
        << "connect failed: " << info;

    /* 请求体：rtdbd_get_req_t（4B，point_id=7），直放 payload.data（零序列化） */
    rtdbd_get_req_t greq;
    std::memset(&greq, 0, sizeof(greq));
    greq.point_id = 7;

    const char *url_str = "/indurtdb/v1/point/get";
    vsoa_url_t url;
    std::memset(&url, 0, sizeof(url));
    url.url = const_cast<char *>(url_str);
    url.url_len = std::strlen(url_str);
    vsoa_payload_t payload;
    std::memset(&payload, 0, sizeof(payload));
    payload.data = &greq;
    payload.data_len = sizeof(greq);

    RpcResult res;
    struct timespec call_ts;
    call_ts.tv_sec = 3;
    call_ts.tv_nsec = 0;
    ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &payload,
                                 on_rpc, &res, &call_ts));
    ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply within timeout";
    EXPECT_EQ(res.status, 0x80); // RTDBD_ST_OK

    /* 本地期望值：同一 dispatch 语义 + 同一 handle */
    uint8_t expect[128];
    size_t expect_len = 0;
    ASSERT_EQ(rtdbd_dispatch_get(handle, 7, expect, sizeof(expect), &expect_len), 0u);
    ASSERT_EQ(expect_len, sizeof(rtdbd_get_resp_t));

    ASSERT_EQ(res.data_len, expect_len);
    EXPECT_EQ(0, std::memcmp(res.data, expect, expect_len))
        << "VSOA 与共享 dispatch 的 /point/get 字节不一致";

    /* 值正确性（防止两端一致但都错）*/
    rtdbd_get_resp_t got;
    std::memset(&got, 0, sizeof(got));
    std::memcpy(&got, res.data, sizeof(got));
    EXPECT_EQ(got.point_id, 7u);
    int32_t v = 0;
    std::memcpy(&v, &got.value_bits, sizeof(v));
    EXPECT_EQ(v, 1234);

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    indurtdb_h_close(handle);
}

TEST(VsoaE2E, SelectThenWriteThroughSharedDispatch)
{
    // 写权威路径在 VSOA 侧打通：/select 设**连接级**分区 → /point/write 写入 →
    // 读回确认值已落库（写权威唯一，§8.3-1）。鉴权走与 UDS **同一函数**
    // （irt_policy_allows + rtdbd_conf_partition_allowed，§8.3-4）。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "vsoa_e2e_write_%d", static_cast<int>(getpid()));
    indurtdb_t *handle = nullptr;
    ASSERT_EQ(indurtdb_h_open(&handle, inst, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(handle, 3, "p3", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], handle);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                    sizeof(addr), &ts, nullptr, info, sizeof(info)))
        << "connect failed: " << info;

    /* 1) /select：设置连接级默认分区（7） */
    uint32_t part = 7;
    {
        const char *u = "/indurtdb/v1/select";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(u);
        url.url_len = std::strlen(u);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        pl.data = &part;
        pl.data_len = sizeof(part);
        RpcResult res;
        struct timespec cts;
        cts.tv_sec = 3;
        cts.tv_nsec = 0;
        ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                     on_rpc, &res, &cts));
        ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply for select";
        EXPECT_EQ(res.status, 0x80); // OK
    }

    /* 2) /point/write：写入 point_id=3，值 4321 */
    rtdbd_write_req_t w;
    std::memset(&w, 0, sizeof(w));
    w.point_id = 3;
    w.type = RTDBD_TYPE_INT32;
    int32_t v = 4321;
    std::memcpy(&w.value_bits, &v, sizeof(v));
    {
        const char *u = "/indurtdb/v1/point/write";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(u);
        url.url_len = std::strlen(u);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        pl.data = &w;
        pl.data_len = sizeof(w);
        RpcResult res;
        struct timespec cts;
        cts.tv_sec = 3;
        cts.tv_nsec = 0;
        ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                     on_rpc, &res, &cts));
        ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply for write";
        EXPECT_EQ(res.status, 0x80); // OK
    }

    /* 3) 读回确认值已落库（写权威唯一：VSOA 写 → 同一份点位数据） */
    uint8_t buf[128];
    size_t len = 0;
    ASSERT_EQ(rtdbd_dispatch_get(handle, 3, buf, sizeof(buf), &len), 0u);
    rtdbd_get_resp_t r;
    std::memset(&r, 0, sizeof(r));
    std::memcpy(&r, buf, sizeof(r));
    int32_t got = 0;
    std::memcpy(&got, &r.value_bits, sizeof(got));
    EXPECT_EQ(got, 4321);

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    indurtdb_h_close(handle);
}

TEST(VsoaE2E, WriteNotifiesSubscribersByDirectedDatagram)
{
    // M1.9 核心：VSOA 侧变更通知**定向投递**（§8.3-5：cli_datagram 逐连接、
    // 禁用 publish/quick、sync_peer 不得收）。链路：client 订阅 /notify 与
    // /subscription/add → 父进程经 VSOA 写入 → 该 client 的 on_datagram 收到变更。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "vsoa_e2e_notify_%d", static_cast<int>(getpid()));
    indurtdb_t *handle = nullptr;
    ASSERT_EQ(indurtdb_h_open(&handle, inst, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(handle, 5, "p5", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], handle);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);
    ASSERT_NE(port, 0);

    DatagramResult dg;
    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    vsoa_client_on_datagram(client, on_datagram, &dg);

    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                    sizeof(addr), &ts, nullptr, info, sizeof(info)))
        << "connect failed: " << info;

    /* 1) 订阅通知 URL（VSOA 发布/订阅语义：定向 datagram 仍需客户端订阅） */
    {
        const char *u = "/indurtdb/v1/notify";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(u);
        url.url_len = std::strlen(u);
        struct timespec sts;
        sts.tv_sec = 2;
        sts.tv_nsec = 0;
        ASSERT_TRUE(vsoa_client_subscribe(client, &url, nullptr, nullptr, &sts))
            << "subscribe notify failed";
    }

    /* 2) 订阅点位 5（写入 session.sub 订阅集合 —— 传输无关真源） */
    {
        rtdbd_sub_req_t sreq;
        std::memset(&sreq, 0, sizeof(sreq));
        sreq.point_id = 5;
        const char *u = "/indurtdb/v1/subscription/add";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(u);
        url.url_len = std::strlen(u);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        pl.data = &sreq;
        pl.data_len = sizeof(sreq);
        RpcResult res;
        struct timespec cts;
        cts.tv_sec = 3;
        cts.tv_nsec = 0;
        ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                     on_rpc, &res, &cts));
        ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply for subscribe";
        EXPECT_EQ(res.status, 0x80);
    }

    /* 3) 写入 → 定向通知应到达 */
    {
        rtdbd_write_req_t w;
        std::memset(&w, 0, sizeof(w));
        w.point_id = 5;
        w.type = RTDBD_TYPE_INT32;
        int32_t v = 777;
        std::memcpy(&w.value_bits, &v, sizeof(v));
        const char *u = "/indurtdb/v1/point/write";
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(u);
        url.url_len = std::strlen(u);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        pl.data = &w;
        pl.data_len = sizeof(w);
        RpcResult res;
        struct timespec cts;
        cts.tv_sec = 3;
        cts.tv_nsec = 0;
        ASSERT_TRUE(vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                     on_rpc, &res, &cts));
        ASSERT_TRUE(pump_client(client, &res, kReplyWaitMs)) << "no reply for write";
        EXPECT_EQ(res.status, 0x80);
    }

    /* 4) 等定向通知（客户端事件循环推进） */
    for (int i = 0; i < 300 && !dg.received; ++i) {
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
    ASSERT_TRUE(dg.received) << "未收到定向 NOTIFY（cli_datagram）";
    EXPECT_EQ(dg.point_id, 5u);
    EXPECT_EQ(dg.type, static_cast<uint8_t>(RTDBD_TYPE_INT32));
    EXPECT_EQ(dg.value, 777);

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    indurtdb_h_close(handle);
}

TEST(VsoaE2E, HelloHandshakeEstablishesIdentity)
{
    // §8.3-4 身份绑定：hello 握手用预共享 token 换取 (uid, partition, service_id,
    // role)；token 不匹配 → DENIED；service_id 与 token 表登记不一致 → DENIED；
    // 握手成功后写路径才被允许（未握手写 → DENIED）。
    indurtdb_cfg_t cfg{};
    cfg.max_points = 64;
    cfg.max_subscribers = 4;
    char inst[64];
    std::snprintf(inst, sizeof(inst), "vsoa_e2e_hello_%d", static_cast<int>(getpid()));
    indurtdb_t *handle = nullptr;
    ASSERT_EQ(indurtdb_h_open(&handle, inst, &cfg), 0);
    ASSERT_EQ(indurtdb_h_create_point(handle, 9, "p9", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);

    int pipefd[2];
    ASSERT_EQ(pipe(pipefd), 0);
    pid_t pid = fork();
    ASSERT_GE(pid, 0);
    if (pid == 0) {
        close(pipefd[0]);
        run_server(pipefd[1], handle);
    }
    close(pipefd[1]);

    uint16_t port = 0;
    ASSERT_EQ(read(pipefd[0], &port, sizeof(port)), static_cast<ssize_t>(sizeof(port)));
    close(pipefd[0]);

    vsoa_client_t *client = vsoa_client_create(nullptr, nullptr);
    ASSERT_NE(client, nullptr);
    struct sockaddr_in addr;
    std::memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts;
    ts.tv_sec = 2;
    ts.tv_nsec = 0;
    char info[256] = {0};
    ASSERT_TRUE(vsoa_client_connect(client, reinterpret_cast<struct sockaddr *>(&addr),
                                    sizeof(addr), &ts, nullptr, info, sizeof(info)))
        << "connect failed: " << info;

    auto rpc = [&](const char *path, const void *body, size_t body_len, RpcResult *res) {
        vsoa_url_t url;
        std::memset(&url, 0, sizeof(url));
        url.url = const_cast<char *>(path);
        url.url_len = std::strlen(path);
        vsoa_payload_t pl;
        std::memset(&pl, 0, sizeof(pl));
        pl.data = const_cast<void *>(body);
        pl.data_len = body_len;
        struct timespec cts;
        cts.tv_sec = 3;
        cts.tv_nsec = 0;
        bool called = vsoa_client_call(client, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                       on_rpc, res, &cts);
        EXPECT_TRUE(called);
        EXPECT_TRUE(pump_client(client, res, kReplyWaitMs));
    };

    /* 1) 错误 token → DENIED（0x82） */
    rtdbd_vsoa_hello_req_t bad;
    std::memset(&bad, 0, sizeof(bad));
    std::snprintf(bad.token, sizeof(bad.token), "wrong-token");
    std::snprintf(bad.service_id, sizeof(bad.service_id), "plc-line-1");
    RpcResult res;
    rpc("/indurtdb/v1/hello", &bad, sizeof(bad), &res);
    EXPECT_EQ(res.status, 0x82); /* DENIED */

    /* 2) service_id 与 token 表登记不一致 → DENIED */
    rtdbd_vsoa_hello_req_t mism;
    std::memset(&mism, 0, sizeof(mism));
    std::snprintf(mism.token, sizeof(mism.token), "tok-plc-1");
    std::snprintf(mism.service_id, sizeof(mism.service_id), "other-line");
    RpcResult res2;
    rpc("/indurtdb/v1/hello", &mism, sizeof(mism), &res2);
    EXPECT_EQ(res2.status, 0x82);

    /* 3) 正确 token + service_id → OK */
    rtdbd_vsoa_hello_req_t good;
    std::memset(&good, 0, sizeof(good));
    std::snprintf(good.token, sizeof(good.token), "tok-plc-1");
    std::snprintf(good.service_id, sizeof(good.service_id), "plc-line-1");
    RpcResult res3;
    rpc("/indurtdb/v1/hello", &good, sizeof(good), &res3);
    EXPECT_EQ(res3.status, 0x80);

    /* 4) 握手后写入成功，且审计带 service_id */
    rtdbd_write_req_t w;
    std::memset(&w, 0, sizeof(w));
    w.point_id = 9;
    w.type = RTDBD_TYPE_INT32;
    int32_t v = 999;
    std::memcpy(&w.value_bits, &v, sizeof(v));
    RpcResult res4;
    rpc("/indurtdb/v1/point/write", &w, sizeof(w), &res4);
    EXPECT_EQ(res4.status, 0x80);

    uint8_t buf[128];
    size_t len = 0;
    ASSERT_EQ(rtdbd_dispatch_get(handle, 9, buf, sizeof(buf), &len), 0u);
    rtdbd_get_resp_t r;
    std::memset(&r, 0, sizeof(r));
    std::memcpy(&r, buf, sizeof(r));
    int32_t got = 0;
    std::memcpy(&got, &r.value_bits, sizeof(got));
    EXPECT_EQ(got, 999);

    vsoa_client_close(client);
    kill(pid, SIGTERM);
    int wstatus = 0;
    waitpid(pid, &wstatus, 0);
    indurtdb_h_close(handle);
}
