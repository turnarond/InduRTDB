/**
 * @file rtdbd_device_agent.c
 * @brief h-C 设备侧代理实现（见 rtdbd_device_agent.h）。
 */
#include "rtdbd_device_agent.h"

#include <rtdbd/protocol.h>
#include "dispatch/rtdbd_dispatch.h" /* rtdbd_st_to_vsoa_status */
#include <vsoa_server.h>
#include <vsoa_client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/select.h>

/* pending 表：并发请求按 cli_id+seq+tun 索引，避免单 pending 覆盖串台（P0-1 fix）。 */
#define RTDBD_AGENT_PENDING_MAX 16u
#define RTDBD_AGENT_MAX_BODY    8192u  /* 与 VSOA resp 上限一致（终验建议③：copied_body 有界） */
#define RTDBD_AGENT_RPC_TIMEOUT_MS 3000u

typedef enum {
    ROUTE_PLC = 0,    /* client#1：4 只读 URL + 13 镜像只读子集 + 订阅管理 */
    ROUTE_ADMIN = 1,  /* client#2：仅 /sync/peers/get */
    ROUTE_INVALID = 2
} route_kind_t;

typedef struct rtdbd_device_agent rtdbd_device_agent_t;
typedef struct {
    bool used;
    rtdbd_device_agent_t *owner;   /* 回写所需的 agent（避免全局） */
    vsoa_cli_id_t cli_id;
    uint32_t seq;
    uint16_t tun;
    route_kind_t route;
    uint8_t body[RTDBD_AGENT_MAX_BODY];
    size_t body_len;
    uint64_t deadline_ns;
} pending_t;

struct rtdbd_device_agent {
    rtdbd_agent_cfg_t cfg;
    vsoa_server_t *server;
    vsoa_client_t *cli_plc;
    vsoa_client_t *cli_admin;
    pending_t pending[RTDBD_AGENT_PENDING_MAX];
    agent_rbe_state_t rbe;
    /* 对外 cli_id ↔ rbe listener idx 映射（vsoa_cli_id_t 是不透明句柄，不能直接做下标）。 */
    vsoa_cli_id_t listeners[RTDBD_AGENT_LISTENER_MAX];
    bool listener_used[RTDBD_AGENT_LISTENER_MAX];
    rtdbd_agent_stats_t stats;
    uint8_t resp[RTDBD_AGENT_MAX_BODY];
};

static uint64_t agent_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* RBE 适配纯逻辑已拆到 rtdbd_agent_rbe.c（OFF 单测可直测，无 SDK 依赖）。 */

/* ---- 代理骨架（双角色 + pending 表 + 透传）---- */
/* URL 路由：只读/订阅/peers get → 对应上游 client；写/peers set → 拒绝。 */
static route_kind_t route_of(const char *bare)
{
    if (!bare) return ROUTE_INVALID;
    if (strcmp(bare, "sync/peers/get") == 0) return ROUTE_ADMIN;
    static const char *const ro[] = {
        "ping", "point/get", "point/batch_read", "caps", "health",
        "point/find_by_name", "point/meta", "point/list", "logs",
        "subscription/add", "subscription/remove", "subscription/set"
    };
    for (size_t i = 0; i < sizeof(ro)/sizeof(ro[0]); ++i) {
        if (strcmp(bare, ro[i]) == 0) return ROUTE_PLC;
    }
    return ROUTE_INVALID;
}

static void pending_reply(rtdbd_device_agent_t *a, vsoa_cli_id_t id,
                          uint32_t seq, uint16_t tun, uint8_t status,
                          const uint8_t *body, size_t len)
{
    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    if (body && len > 0u && len <= sizeof(a->resp)) {
        memcpy(a->resp, body, len);
        out.data = a->resp;
        out.data_len = len;
    }
    (void)vsoa_server_cli_reply(a->server, id, status, seq, tun, &out);
    a->stats.replied++;
}

/* 上游 RPC 回调：按 cli_id+seq+tun 精确查 pending 表回写，避免并发串台。 */
static void on_upstream_rpc(void *arg, vsoa_client_t *c, vsoa_header_t *hdr,
                            vsoa_payload_t *pl)
{
    (void)c; (void)hdr;
    /* 每个 client_call 直接携带自己的 pending_t* 作 arg——无查找、无串台
     * （不同外部 cli 的 seq 空间独立，按 seq+tun 查会冲突）。 */
    pending_t *p = (pending_t *)arg;
    if (!p || !p->owner) return;
    rtdbd_device_agent_t *a = p->owner;
    uint8_t status = 0x83;
    const uint8_t *body = NULL;
    size_t len = 0u;
    if (pl) {
        status = hdr ? hdr->status : 0x83;
        body = pl->data_len > 0u ? (const uint8_t *)pl->data : NULL;
        len = pl->data_len > RTDBD_AGENT_MAX_BODY ? RTDBD_AGENT_MAX_BODY : pl->data_len;
    }
    pending_reply(a, p->cli_id, p->seq, p->tun, status, body, len);
    memset(p, 0, sizeof(*p));
}

/* 上游 NOTIFY datagram → 按 per-listener 订阅集二次过滤扇出（RBE 适配层）。 */
static void on_upstream_datagram(void *arg, vsoa_client_t *c,
                                 vsoa_url_t *url, vsoa_payload_t *pl, bool quick)
{
    (void)c; (void)url; (void)quick;
    rtdbd_device_agent_t *a = (rtdbd_device_agent_t *)arg;
    if (!a || !pl || !pl->data || pl->data_len < sizeof(rtdbd_notify_t)) return;
    rtdbd_notify_t nt;
    memcpy(&nt, pl->data, sizeof(nt)); /* 回调指针返回即失效：拷贝 */
    agent_rbe_cache(&a->rbe, nt.point_id, (const uint8_t *)&nt);

    vsoa_url_t durl;
    memset(&durl, 0, sizeof(durl));
    char du[] = "/indurtdb/v1/notify"; durl.url = du; durl.url_len = strlen(du);
    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    out.data = &nt; out.data_len = sizeof(nt);
    for (uint32_t lid = 0; lid < RTDBD_AGENT_LISTENER_MAX; ++lid) {
        if (a->listener_used[lid] && agent_rbe_listener_has(&a->rbe, lid, nt.point_id)) {
            (void)vsoa_server_cli_datagram(a->server, a->listeners[lid], &durl, &out);
        }
    }
}

/* cli_id → rbe listener idx 映射；新 cli 分配空闲 idx，满则拒绝（u-E≤16）。 */
static int listener_idx_for(rtdbd_device_agent_t *a, vsoa_cli_id_t id)
{
    for (uint32_t i = 0; i < RTDBD_AGENT_LISTENER_MAX; ++i) {
        if (a->listener_used[i] && a->listeners[i] == id) return (int)i;
    }
    uint32_t lim = a->cfg.max_listeners ? a->cfg.max_listeners : RTDBD_AGENT_LISTENER_MAX;
    for (uint32_t i = 0; i < lim; ++i) {
        if (!a->listener_used[i]) {
            a->listener_used[i] = true;
            a->listeners[i] = id;
            return (int)i;
        }
    }
    return -1; /* 并发 listener 满（第17个） */
}

/* 对外 server 回调：拷贝 POD、转字节序、登记 pending、异步转发（不阻塞）。 */
static void on_external_cmd(void *arg, vsoa_server_t *s, vsoa_cli_id_t id,
                            vsoa_header_t *hdr, vsoa_url_t *url, vsoa_payload_t *pl)
{
    (void)s;
    rtdbd_device_agent_t *a = (rtdbd_device_agent_t *)arg;
    if (!a || !url || !url->url) return;

    /* 归一化 URL（去 /indurtdb/v1 前缀）：与 adapter 的 rtdbd_normalize_url 同语义。 */
    char raw[128];
    size_t n = url->url_len < sizeof(raw) - 1u ? url->url_len : sizeof(raw) - 1u;
    memcpy(raw, url->url, n);
    raw[n] = '\0';
    const char *bare = strstr(raw, "/v1/");
    bare = bare ? bare + 4u : raw;
    if (*bare == '/') bare++;

    route_kind_t route = route_of(bare);
    if (route == ROUTE_INVALID) {
        a->stats.denied++;
        pending_reply(a, id, hdr ? ntohl(hdr->seqno) : 0u,
                      hdr ? ntohs(hdr->tunid) : 0u,
                      (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_NOT_IMPLEMENTED), NULL, 0u);
        return;
    }

    /* listener 映射（u-E：并发上限 16，第 17 个拒绝）。 */
    int lidx = listener_idx_for(a, id);
    if (lidx < 0) {
        a->stats.denied++;
        pending_reply(a, id, hdr ? ntohl(hdr->seqno) : 0u,
                      hdr ? ntohs(hdr->tunid) : 0u,
                      (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_DENIED), NULL, 0u);
        return;
    }
    /* 订阅管理 URL：更新 per-listener 订阅集 + 引用计数（RBE 适配层）。
     * 首个 listener 订阅点 → 向上游订阅；最后 listener 退订 → 向上游退订。
     * 这里只更新本地状态与记录待转发标志；实际转发在下方 pending + client_call 完成。 */
    if (pl && pl->data_len >= sizeof(rtdbd_sub_req_t) && pl->data &&
        (strcmp(bare, "subscription/add") == 0 || strcmp(bare, "subscription/remove") == 0)) {
        rtdbd_sub_req_t sr;
        memcpy(&sr, pl->data, sizeof(sr));
        if (strcmp(bare, "subscription/add") == 0) {
            (void)agent_rbe_subscribe(&a->rbe, (uint32_t)lidx, sr.point_id);
        } else {
            (void)agent_rbe_unsubscribe(&a->rbe, (uint32_t)lidx, sr.point_id);
        }
    }

    uint32_t seq = hdr ? ntohl(hdr->seqno) : 0u;
    uint16_t tun = hdr ? ntohs(hdr->tunid) : 0u;
    /* 找空闲 pending 项（超时项先回收）。 */
    uint64_t now = agent_now_ns();
    pending_t *p = NULL;
    for (uint32_t i = 0; i < RTDBD_AGENT_PENDING_MAX; ++i) {
        if (!a->pending[i].used) { p = &a->pending[i]; break; }
        if (a->pending[i].deadline_ns <= now) memset(&a->pending[i], 0, sizeof(pending_t));
    }
    if (!p) {
        a->stats.pending_full++;
        pending_reply(a, id, seq, tun,
                      (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_INTERNAL), NULL, 0u);
        return;
    }
    size_t body_len = pl && pl->data_len > 0u ? pl->data_len : 0u;
    if (body_len > RTDBD_AGENT_MAX_BODY) body_len = RTDBD_AGENT_MAX_BODY;
    p->used = true; p->owner = a; p->cli_id = id; p->seq = seq; p->tun = tun; p->route = route;
    p->body_len = body_len;
    if (body_len > 0u) memcpy(p->body, pl->data, body_len); /* 回调指针返回即失效：拷贝 */
    p->deadline_ns = now + (uint64_t)RTDBD_AGENT_RPC_TIMEOUT_MS * 1000000ull;

    vsoa_client_t *up = route == ROUTE_ADMIN ? a->cli_admin : a->cli_plc;
    vsoa_url_t u;
    memset(&u, 0, sizeof(u));
    /* 转发用原始完整 URL（/indurtdb/v1/xxx），与 rtdbd 注册 listener 一致；
     * 归一化 full 仅用于本地 route_of 判定。 */
    u.url = raw; u.url_len = strlen(raw);
    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    if (body_len > 0u) { out.data = p->body; out.data_len = body_len; }
    struct timespec ts;
    ts.tv_sec = RTDBD_AGENT_RPC_TIMEOUT_MS / 1000u; ts.tv_nsec = 0;
    if (!vsoa_client_call(up, VSOA_CLIENT_RPC_METHOD_GET, &u, &out,
                          on_upstream_rpc, p, &ts)) {
        memset(p, 0, sizeof(*p));
        a->stats.denied++;
        pending_reply(a, id, seq, tun,
                      (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_INTERNAL), NULL, 0u);
    }
}

/* 对外注册的 URL 白名单（精确匹配，仿 rtdbd adapter 逐个 add_listener）。
 * 与 route_of 的白名单一致：13 镜像只读子集 + 4 只读 + 订阅管理 + peers get。 */
static const char *const AGENT_LISTEN_URLS[] = {
    "/indurtdb/v1/ping", "/indurtdb/v1/point/get", "/indurtdb/v1/point/batch_read",
    "/indurtdb/v1/caps", "/indurtdb/v1/health",
    "/indurtdb/v1/point/find_by_name", "/indurtdb/v1/point/meta",
    "/indurtdb/v1/point/list", "/indurtdb/v1/logs",
    "/indurtdb/v1/subscription/add", "/indurtdb/v1/subscription/remove",
    "/indurtdb/v1/subscription/set", "/indurtdb/v1/sync/peers/get"
};

/* hello POD（与 vsoa_adapter.h 同构，避免引入整个 adapter 头）。 */
typedef struct { char token[64]; char service_id[64]; } agent_hello_req_t;
typedef struct { uint64_t session_id; } agent_hello_resp_t;

typedef struct { bool done; uint8_t status; } hello_ctx_t;
static void on_hello(void *arg, vsoa_client_t *c, vsoa_header_t *hdr, vsoa_payload_t *pl)
{
    (void)c; (void)pl;
    hello_ctx_t *hc = (hello_ctx_t *)arg;
    if (hc && hdr) { hc->status = hdr->status; hc->done = true; }
}

/* create 阶段同步 hello：尚未进入主循环，用 client fds 本地 poll 等待 reply。 */
static bool agent_hello_sync(vsoa_client_t *c, const char *token, const char *service_id)
{
    agent_hello_req_t req;
    memset(&req, 0, sizeof(req));
    strncpy(req.token, token, sizeof(req.token) - 1u);
    strncpy(req.service_id, service_id, sizeof(req.service_id) - 1u);
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    char us[] = "/indurtdb/v1/hello"; u.url = us; u.url_len = strlen(us);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = &req; pl.data_len = sizeof(req);
    hello_ctx_t hc; memset(&hc, 0, sizeof(hc));
    struct timespec ts; ts.tv_sec = 3; ts.tv_nsec = 0;
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl, on_hello, &hc, &ts)) {
        return false;
    }
    for (int i = 0; i < 300 && !hc.done; ++i) {
        fd_set rfds; FD_ZERO(&rfds);
        vsoa_client_fds(c, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0)
            vsoa_client_input_fds(c, &rfds);
    }
    return hc.done && hc.status == 0x80;
}

rtdbd_device_agent_t *rtdbd_device_agent_create(const rtdbd_agent_cfg_t *cfg)
{
    if (!cfg || !cfg->bind_ip || !cfg->passwd || cfg->max_listeners > RTDBD_AGENT_LISTENER_MAX) {
        return NULL;
    }
    if (strcmp(cfg->bind_ip, "0.0.0.0") == 0) return NULL; /* d-A：拒绝全网卡 */
    rtdbd_device_agent_t *a = (rtdbd_device_agent_t *)calloc(1, sizeof(*a));
    if (!a) return NULL;
    a->cfg = *cfg;
    agent_rbe_init(&a->rbe);

    a->server = vsoa_server_create("{\"name\":\"rtdbd-device-agent\"}");
    if (!a->server) goto fail;
    if (!vsoa_server_passwd(a->server, cfg->passwd)) goto fail;
    /* 对外 server 逐个注册白名单 URL（VSOA listener 精确匹配，仿 adapter）。
     * 非白名单 URL（含写类/peers set）SDK 回 status=3，等价于拒绝。 */
    for (size_t i = 0; i < sizeof(AGENT_LISTEN_URLS)/sizeof(AGENT_LISTEN_URLS[0]); ++i) {
        vsoa_url_t u;
        memset(&u, 0, sizeof(u));
        u.url = (char *)AGENT_LISTEN_URLS[i];
        u.url_len = strlen(AGENT_LISTEN_URLS[i]);
        if (!vsoa_server_add_listener(a->server, &u, on_external_cmd, a)) goto fail;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(cfg->port);
    addr.sin_addr.s_addr = inet_addr(cfg->bind_ip); /* d-A：指定管理网卡 IP */
    if (!vsoa_server_start(a->server, (struct sockaddr *)&addr, sizeof(addr))) goto fail;
    /* port-file 回读（独立 RTDBD_AGENT_PORT_FILE）。 */
    if (cfg->port_file) {
        struct sockaddr_in actual;
        socklen_t sl = sizeof(actual);
        memset(&actual, 0, sizeof(actual));
        if (!vsoa_server_address(a->server, (struct sockaddr *)&actual, &sl)) goto fail;
        FILE *f = fopen(cfg->port_file, "w");
        if (!f) goto fail;
        fprintf(f, "%u\n", (unsigned)ntohs(actual.sin_port));
        fclose(f);
    }

    /* 2 条 loopback client（d-B）：先连 rtdbd，再 hello 由上游 token 校验。 */
    struct sockaddr_in up;
    memset(&up, 0, sizeof(up));
    up.sin_family = AF_INET;
    up.sin_port = htons(cfg->upstream_port);
    up.sin_addr.s_addr = inet_addr(cfg->upstream_host);
    struct timespec ts; ts.tv_sec = 3; ts.tv_nsec = 0;
    char info[256] = {0};
    /* client_create 的 onmsg 是 datagram 回调（收上游 NOTIFY）；RPC 回调用 call 参数。 */
    a->cli_plc = vsoa_client_create(on_upstream_datagram, a);
    a->cli_admin = vsoa_client_create(NULL, NULL);
    if (!a->cli_plc || !a->cli_admin) goto fail;
    if (!vsoa_client_connect(a->cli_plc, (struct sockaddr *)&up, sizeof(up),
                             &ts, NULL, info, sizeof(info))) goto fail;
    if (!vsoa_client_connect(a->cli_admin, (struct sockaddr *)&up, sizeof(up),
                             &ts, NULL, info, sizeof(info))) goto fail;
    /* 两条 client 同步 hello（rtdbd 在 hello 阶段校验 token+service_id+loopback）。
     * plc token 读 4 URL/镜像/订阅，sync_admin token 仅 peers get——role 互斥、双 token。 */
    if (!agent_hello_sync(a->cli_plc, cfg->token_ro, "agent-ro")) goto fail;
    if (!agent_hello_sync(a->cli_admin, cfg->token_admin, "agent-admin")) goto fail;
    return a;
fail:
    rtdbd_device_agent_destroy(a);
    return NULL;
}

void rtdbd_device_agent_destroy(rtdbd_device_agent_t *a)
{
    if (!a) return;
    if (a->cli_admin) vsoa_client_close(a->cli_admin);
    if (a->cli_plc) vsoa_client_close(a->cli_plc);
    if (a->server) vsoa_server_close(a->server);
    free(a->rbe.points);
    free(a);
}

bool rtdbd_device_agent_poll_once(rtdbd_device_agent_t *a, int timeout_ms)
{
    if (!a) return false;
    fd_set rfds;
    FD_ZERO(&rfds);
    vsoa_server_fds(a->server, &rfds);
    vsoa_client_fds(a->cli_plc, &rfds);
    vsoa_client_fds(a->cli_admin, &rfds);
    struct timeval tv;
    tv.tv_sec = timeout_ms / 1000;
    tv.tv_usec = (timeout_ms % 1000) * 1000;
    if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) <= 0) return false;
    vsoa_server_input_fds(a->server, &rfds);
    vsoa_client_input_fds(a->cli_plc, &rfds);
    vsoa_client_input_fds(a->cli_admin, &rfds);
    return true;
}

rtdbd_agent_stats_t rtdbd_device_agent_get_stats(const rtdbd_device_agent_t *a)
{
    if (!a) { rtdbd_agent_stats_t z; memset(&z, 0, sizeof(z)); return z; }
    return a->stats;
}
