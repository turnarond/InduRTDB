/**
 * @file vsoa_adapter.c
 * @brief VSOA 前端适配层实现（M1.3b）：VSOA 帧 ⇄ transport 无关 dispatch 桥接。
 *
 * 关键约束（doc 13）：
 * - §7.2：status 用 0x80 + RTDBD_ST_*（rtdbd_st_to_vsoa_status）。
 * - §9.3：vsoa_payload_t.data 直放定长 POD（零序列化）；NOTIFY/SNAPSHOT_DONE 走
 *   cli_datagram 定向、禁用 publish；quick channel 禁承载增量/快照/ack/握手
 *   （本文件只用 RPC 通道，未使用 quick）。
 * - §8.3-3：命令回调在 vsoa_server_input_fds() 内被调用，故回调内不得阻塞、
 *   不得 reconnect（这类动作须走延迟动作队列）。
 */
#include "vsoa_adapter.h"

#include "dispatch/rtdbd_dispatch.h"
#include "cluster/rtdbd_peers_global.h"
#include "session/rtdbd_session.h" /* per-cli 传输无关会话（§8.3-3） */
#include "session/rtdbd_authz.h"   /* §8.3-4 身份/来源授权判据（M3.x P7） */
#include "cluster/rtdbd_sub_snapshot.h" /* h-A：断链订阅快照（session_id 键） */
#include "stats.h"                      /* h-B：g_stats（health 计数快照） */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/select.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>
#include <vsoa_server.h>
#include "vsoa_sync_protocol.h"   /* S2：同步通道专有 POD */
#include "cluster/rtdbd_sync_logic.h" /* S2：delta 扫描/WAIT 阈值纯逻辑 */
#include "cluster/rtdbd_epoch.h"      /* S1：全局 epoch */
#include "cluster/rtdbd_digest.h"     /* S1：点集摘要 */

/* 响应体上限：与既有 UDS 侧单帧负载量级一致（快照走分页 RPC，不在此限内）。 */
/* M3.3 切片 C：/sync/peers/get 回 4104B 定长 POD（rtdbd_peers_resp_t），
 * 原 4096B 装不下；扩容并留出余量。 */
#define RTDBD_VSOA_RESP_MAX 8192u

/* S2：per-cli 同步状态（hello 交换后登记，断链清零）。 */
typedef struct {
    uint64_t epoch;
    uint64_t struct_hash;
    uint64_t value_digest;
    uint32_t n_points;
    char     node_id[RTDBD_PEER_NODE_ID_MAX];
    bool     valid;
} rtdbd_sync_peer_state_t;

/* S2：WAIT 延迟等待上下文（回调内只登记，pump 内异步回写；终验 P0-1）。 */
typedef struct {
    bool            used;
    vsoa_cli_id_t   cli_id;
    uint32_t        seq;      /* 主机序（ntohl 后，终验 P1） */
    uint16_t        tun;      /* 主机序（ntohs 后） */
    uint64_t        min_epoch;
    uint32_t        required_acks;
    uint64_t        deadline_ns; /* CLOCK_MONOTONIC ns */
} rtdbd_sync_wait_ctx_t;

/* per-cli 会话项类型（S2 同步状态随之挂载）。 */
typedef struct {
    vsoa_cli_id_t          id;
    bool                   used;
    rtdbd_session_t        session;
    rtdbd_sync_peer_state_t sync_state;
} rtdbd_vsoa_cli_entry_t;

struct rtdbd_vsoa_adapter {
    vsoa_server_t     *server;
    rtdbd_vsoa_handler_t handler;
    uint16_t           port;
    /* 服务端能力配置快照（/caps）：与 UDS 侧共用同一 dispatch 实现，
     * 故两前端产出的 32B 响应体字节一致。 */
    rtdbd_dispatch_cfg_t cfg;
    bool                 cfg_set;
    /* 点位库句柄（读点能力用）：由 rtdbd 注入真实 handle，两前端读同一份数据。 */
    indurtdb_t          *handle;
    /* 写路径依赖（§8.3-4：与 UDS 同一鉴权函数、同一 deny-by-default）。 */
    const irt_policy_t  *policy;
    const rtdbd_conf_t  *conf;
    rtdbd_vsoa_audit_fn  audit;
    void                *audit_ctx;
    uint32_t             default_uid;
    bool                 allow_unauthenticated; /* 仅本机联调（默认关闭） */
    /* 预共享 token 表（§8.3-4：32 条上限、零动态分配） */
    rtdbd_vsoa_token_entry_t tokens[RTDBD_VSOA_MAX_TOKEN];
    /* per-cli 会话表（传输无关会话按连接隔离，§8.3-3/§8.3-4）：
     * partition 等连接级状态挂在这里；连接建立/断开由 on_cli 回调维护。 */
    rtdbd_vsoa_cli_entry_t cli[RTDBD_VSOA_MAX_CLI];
    /* S2：WAIT 等待表 + 本机 node_id（--sync-node-id，未给取 service_id）。 */
    rtdbd_sync_wait_ctx_t wait_ctx[RTDBD_SYNC_WAIT_MAX];
    char                  sync_node_id[RTDBD_PEER_NODE_ID_MAX];
    /* v3.12 M3.3 切片4：NOTIFY 门控诊断计数（adapter 全局聚合，非 per-session）。
     * calloc 创建时自动零初始化；纯标量、单线程普通 ++（doc 13 §8.3-3）。 */
    rtdbd_notify_counters_t notify_counters;
    /* h-A：服务端稳定 session_id 生成器（单调递增、非零；hello 成功响应下发）。 */
    uint64_t           next_session_id;
    /* h-C 前置：只读 URL 数据源（rtdbd main 注入；未注入回 NOT_IMPLEMENTED）。 */
    const irt_logbuf_t *readonly_logbuf;
    uint32_t            readonly_max_points;
    /* 回调内使用的响应缓冲：SDK 规定 payload 在回调返回后失效，故必须自备。 */
    uint8_t            resp[RTDBD_VSOA_RESP_MAX];
};

/* 13 个镜像能力（doc 13 §7.1）；注册时补版本段前缀。 */
static const char *const VSOA_MIRROR_URLS[] = {
    "/ping", "/point/write", "/point/batch_write", "/point/get", "/point/batch_read",
    "/subscription/add", "/subscription/remove", "/subscription/set",
    "/notify", "/snapshot_done", "/caps", "/select", "/health",
};
#define VSOA_MIRROR_COUNT (sizeof(VSOA_MIRROR_URLS) / sizeof(VSOA_MIRROR_URLS[0]))

/* per-cli 会话查找（线性扫描：连接数上限 32，且只在命令路径上调用）。 */
static rtdbd_session_t *cli_session(rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id)
{
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used && a->cli[i].id == id) {
            return &a->cli[i].session;
        }
    }
    return NULL;
}

/* S2：按 cli_id 取整个 cli 项（含 sync_state），供同步 handler 登记状态。 */
static rtdbd_vsoa_cli_entry_t *cli_entry_for(
    rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id)
{
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used && a->cli[i].id == id) return &a->cli[i];
    }
    return NULL;
}

/* h-C 前置：4 只读 URL 的 β 门禁判据（r-A=β）。只封装判据、返回 bool；
 * DENIED reply 由各分支自行发送，避免 helper 承担 server/seq/tun 参数。
 * 语义对齐公共门禁 :519-536：仅 cred_ok && role!=UNKNOWN 的会话受 role→URL 白名单约束，
 * UNKNOWN 联调会话沿用 cred_ok，不误伤既有 allow_unauthenticated e2e。 */
static bool vsoa_readonly_gate_allowed(rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id,
                                       const char *bare)
{
    rtdbd_session_t *s = cli_session(a, id);
    return !(s && s->cred.cred_ok && s->role != RTDBD_ROLE_UNKNOWN &&
             !rtdbd_authz_url_allowed(s->role, bare));
}

/* h-C 前置：4 只读 URL 统一 handler。kind: 0=find 1=meta 2=list 3=log。
 * 门禁 → req_len 校验 → 数据源检查 → 共享纯函数 → 回包，全在 SDK 回调线程内同步完成。 */
typedef enum { HC_RO_FIND = 0, HC_RO_META, HC_RO_LIST, HC_RO_LOG } hc_ro_kind_t;

static void vsoa_readonly_handle(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                                 vsoa_cli_id_t id, vsoa_header_t *hdr,
                                 const char *bare, const uint8_t *req, size_t req_len,
                                 hc_ro_kind_t kind)
{
    uint32_t seq = hdr ? ntohl(hdr->seqno) : 0u;
    uint16_t tun = hdr ? ntohs(hdr->tunid) : 0u;
    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));

    /* r-A=β：门禁在分支内显式执行（公共门禁对非镜像 URL 不可达）。 */
    if (!vsoa_readonly_gate_allowed(a, id, bare)) {
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_DENIED), seq, tun, &out);
        return;
    }

    size_t expect = (kind == HC_RO_FIND) ? sizeof(rtdbd_find_req_t)
                  : (kind == HC_RO_META) ? sizeof(rtdbd_meta_req_t)
                  : (kind == HC_RO_LIST) ? sizeof(rtdbd_list_req_t)
                                         : sizeof(rtdbd_log_req_t);
    if (req_len != expect || !req) { /* 对齐 UDS 各 op_* 的 payload_len 校验 */
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_BAD_REQUEST), seq, tun, &out);
        return;
    }
    /* 数据源检查：未注入 handle/conf/logbuf → NOT_IMPLEMENTED（不糊弄）。 */
    if (!a->handle || !a->conf ||
        (kind == HC_RO_LOG && !a->readonly_logbuf)) {
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_NOT_IMPLEMENTED), seq, tun, &out);
        return;
    }

    uint16_t st = RTDBD_ST_OK;
    size_t body_len = 0u;
    switch (kind) {
    case HC_RO_FIND: {
        rtdbd_find_req_t f;
        memcpy(&f, req, sizeof(f));
        f.name[sizeof(f.name) - 1u] = '\0'; /* 全 0xFF 无 NUL 也安全（对齐 UDS :1210） */
        uint32_t found = 0u;
        st = rtdbd_dispatch_find(a->handle, a->conf, f.partition, f.name, &found);
        if (st == RTDBD_ST_OK) {
            rtdbd_find_resp_t r;
            memset(&r, 0, sizeof(r));
            r.point_id = found;
            memcpy(a->resp, &r, sizeof(r));
            body_len = sizeof(r);
        }
        break;
    }
    case HC_RO_META: {
        rtdbd_meta_req_t m;
        memcpy(&m, req, sizeof(m));
        rtdbd_meta_payload_t meta;
        memset(&meta, 0, sizeof(meta));
        st = rtdbd_dispatch_meta(a->handle, m.point_id, &meta);
        if (st == RTDBD_ST_OK) {
            memcpy(a->resp, &meta, sizeof(meta));
            body_len = sizeof(meta);
        }
        break;
    }
    case HC_RO_LIST: {
        rtdbd_list_req_t l;
        memcpy(&l, req, sizeof(l));
        size_t olen = 0u;
        /* cap=sizeof(a->resp)=8192 → 纯函数内截断 max 到 107 条。 */
        st = rtdbd_dispatch_list_snapshot(a->handle, a->conf, l.partition, l.max,
                                          l.offset, a->readonly_max_points,
                                          a->resp, sizeof(a->resp), &olen);
        if (st == RTDBD_ST_OK) body_len = olen;
        break;
    }
    case HC_RO_LOG: {
        rtdbd_log_req_t q;
        memcpy(&q, req, sizeof(q));
        size_t olen = 0u;
        st = rtdbd_dispatch_log_snapshot(a->readonly_logbuf, q.max,
                                         a->resp, sizeof(a->resp), &olen);
        if (st == RTDBD_ST_OK) body_len = olen;
        break;
    }
    }
    if (body_len > sizeof(a->resp)) { /* 防御：纯函数已按 cap 截断，这里仅兜底 */
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_INTERNAL), seq, tun, &out);
        return;
    }
    if (body_len > 0u) {
        out.data = a->resp;
        out.data_len = body_len;
    }
    (void)vsoa_server_cli_reply(server, id,
        (uint8_t)rtdbd_st_to_vsoa_status(st), seq, tun, &out);
}

/* 连接建立/断开回调：维护 per-cli 会话（§8.3-3 传输无关会话 + §8.3-4 身份绑定）。
 * 断开时释放会话槽，避免连接 id 复用导致状态串台。 */
static void vsoa_on_cli(void *arg, vsoa_server_t *server, vsoa_cli_id_t id, bool connect)
{
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)arg;
    (void)server;
    if (!a) {
        return;
    }
    if (connect) {
        for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
            if (a->cli[i].used) {
                continue;
            }
            if (!rtdbd_session_init(&a->cli[i].session, RTDBD_SUBTAB_MIN_CAP)) {
                return; /* 订阅表初始分配失败：连接仍可建立，但无会话状态 */
            }
            a->cli[i].id = id;
            a->cli[i].used = true;
            a->cli[i].session.cred.uid = a->default_uid;
            a->cli[i].session.cred.cred_ok = (a->default_uid != 0);
            return;
        }
        return; /* 会话表满：不覆盖既有会话 */
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used && a->cli[i].id == id) {
            /* h-A：断链保存订阅快照（session_id 键），重连 rebind 时恢复。
             * 只存点集+身份校验字段，不入队、不在回调内执行恢复。 */
            rtdbd_session_t *s = &a->cli[i].session;
            if (s->session_id != 0u && s->cred.cred_ok) {
                rtdbd_sub_snap_identity_t ident;
                ident.uid = s->cred.uid;
                ident.partition = s->partition;
                memset(ident.service_id, 0, sizeof(ident.service_id));
                if (s->service_id[0] != '\0') {
                    (void)snprintf(ident.service_id, sizeof(ident.service_id), "%s",
                                   s->service_id);
                }
                (void)rtdbd_sub_snapshot_save(s->session_id, &ident, s->subs, s->nsubs);
            }
            rtdbd_session_free(s);
            /* S2（终验 P1）：断链清同步状态 + WAIT 等待表项，防 cli_id 复用串台。 */
            memset(&a->cli[i].sync_state, 0, sizeof(a->cli[i].sync_state));
            for (uint32_t k = 0; k < RTDBD_SYNC_WAIT_MAX; ++k) {
                if (a->wait_ctx[k].used && a->wait_ctx[k].cli_id == a->cli[i].id) {
                    memset(&a->wait_ctx[k], 0, sizeof(a->wait_ctx[k]));
                }
            }
            a->cli[i].used = false;
            a->cli[i].id = 0;
            return;
        }
    }
}

/* hello 握手处理（§8.3-4）：请求体带预共享 token + service_id；服务端按定长
 * token 表映射 (uid, partition, service_id, role) 写入该连接的 rtdbd_session_t，
 * 并置 cli_set_authed（挡 publish 订阅）。
 * ⚠ token 明文比对：token 属链路准入凭据，线上应由 TLS/专线承载（§8.3-4 的
 * vsoa_server_passwd 只作链路准入，不承担按点鉴权）。 */
static uint16_t vsoa_hello(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                            vsoa_cli_id_t id, const uint8_t *req, size_t req_len)
{
    if (req_len != sizeof(rtdbd_vsoa_hello_req_t)) {
        return RTDBD_ST_BAD_REQUEST;
    }
    rtdbd_vsoa_hello_req_t hello;
    memcpy(&hello, req, sizeof(hello));
    hello.token[sizeof(hello.token) - 1] = '\0';
    hello.service_id[sizeof(hello.service_id) - 1] = '\0';

    const rtdbd_vsoa_token_entry_t *ent = NULL;
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used && strcmp(a->tokens[i].token, hello.token) == 0) {
            ent = &a->tokens[i];
            break;
        }
    }
    if (!ent) {
        return RTDBD_ST_DENIED; /* token 不匹配 */
    }

    /* 来源强校验（§8.3-4）：判据下沉到传输无关纯逻辑 rtdbd_authz。
     * - plc / sync_admin 仅 127/8（由 rtdbd_authz_source_allowed 处理）；
     * - sync_peer 必须是已知集群成员（来源 addr 落在运行时成员表 addr 集合，
     *   由 rtdbd_authz_sync_peer_source_allowed 查 rtdbd_peers_global()）。 */
    struct sockaddr_storage peer_addr;
    socklen_t plen = sizeof(peer_addr);
    memset(&peer_addr, 0, sizeof(peer_addr));
    if (!vsoa_server_cli_address(server, id, (struct sockaddr *)&peer_addr, &plen)) {
        return RTDBD_ST_DENIED;
    }
    const bool is_ipv4 = (peer_addr.ss_family == AF_INET);
    uint32_t addr_host = 0;
    char srcbuf[INET_ADDRSTRLEN];
    srcbuf[0] = '\0';
    if (is_ipv4) {
        const struct sockaddr_in *in4 = (const struct sockaddr_in *)&peer_addr;
        addr_host = ntohl(in4->sin_addr.s_addr);
        inet_ntop(AF_INET, &in4->sin_addr, srcbuf, sizeof(srcbuf));
    }
    const uint16_t src_st = (ent->role == RTDBD_ROLE_SYNC_PEER)
        ? rtdbd_authz_sync_peer_source_allowed(rtdbd_peers_global(), srcbuf)
        : rtdbd_authz_source_allowed(ent->role, is_ipv4, addr_host);
    if (src_st != RTDBD_ST_OK) {
        return src_st;
    }

    rtdbd_session_t *sess = cli_session(a, id);
    if (!sess) {
        return RTDBD_ST_NOT_IMPLEMENTED; /* 无会话状态 */
    }
    /* 请求携带的 service_id 必须与 token 表登记一致（防身份混淆） */
    if (strcmp(hello.service_id, ent->service_id) != 0) {
        return RTDBD_ST_DENIED;
    }
    sess->cred.uid = ent->uid;
    sess->cred.cred_ok = true;
    sess->role = ent->role;
    sess->partition = ent->partition;
    (void)rtdbd_session_set_service_id(sess, ent->service_id);
    /* h-A：每次 hello 成功分配新的稳定 session_id（重连标识、非凭据），
     * 由 hello 响应下发给客户端，重连时经 /session/rebind 回传匹配订阅快照。 */
    if (++a->next_session_id == 0u) a->next_session_id = 1u; /* 跳过 0 哨兵 */
    sess->session_id = a->next_session_id;
    /* 握手成功才置 authed（挡 publish 订阅，§8.3-4） */
    (void)vsoa_server_cli_set_authed(server, id, true);
    return RTDBD_ST_OK;
}

/* h-A：/session/rebind —— 重连后回传 session_id，服务端按快照恢复订阅点集。
 * 须已 hello（cred_ok）；身份字段取当前会话（每次 hello 重建），与快照记录
 * 比对一致（纵深防御）才恢复；成功后消费快照（remove 防重复恢复）。 */
static uint16_t vsoa_session_rebind(rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id,
                                    const uint8_t *req, size_t req_len,
                                    uint32_t max_subs)
{
    if (req_len != sizeof(rtdbd_vsoa_session_rebind_t) || !req) {
        return RTDBD_ST_BAD_REQUEST;
    }
    rtdbd_session_t *sess = cli_session(a, id);
    if (!sess || !sess->cred.cred_ok) {
        return RTDBD_ST_DENIED; /* 未 hello 不重放（s-A：身份必须重新建立） */
    }
    rtdbd_vsoa_session_rebind_t rb;
    memcpy(&rb, req, sizeof(rb));
    if (rb.session_id == 0u) {
        return RTDBD_ST_BAD_REQUEST;
    }

    rtdbd_sub_snap_identity_t ident;
    ident.uid = sess->cred.uid;
    ident.partition = sess->partition;
    memset(ident.service_id, 0, sizeof(ident.service_id));
    if (sess->service_id[0] != '\0') {
        (void)snprintf(ident.service_id, sizeof(ident.service_id), "%s",
                       sess->service_id);
    }

    uint32_t ids[RTDBD_SUB_MAX];
    uint32_t n = 0u;
    if (!rtdbd_sub_snapshot_load(rb.session_id, &ident, ids, RTDBD_SUB_MAX, &n)) {
        return RTDBD_ST_NOT_FOUND; /* 未命中/身份不符/超容量：显式失败，不误装 */
    }
    if (!rtdbd_session_install_subs(sess, ids, n)) {
        return RTDBD_ST_INTERNAL;
    }
    (void)rtdbd_sub_snapshot_remove(rb.session_id);
    (void)max_subs; /* install_subs 内部受 RTDBD_SUB_MAX 限，max_subs 由调用方配置 */
    return RTDBD_ST_OK;
}

/* v3.12 M3.3 切片4：RBE 判定回调包装。rtdbd_dispatch_notify_rbe 有副作用
 * （推进 subtab 状态），由 session 层门控纯函数在「sync_peer / 未订阅」过滤后
 * 才调用，保证未订阅 / 回环连接不消耗 RBE 状态（与 UDS 侧同顺序）。 */
typedef struct {
    const rtdbd_write_req_t *w;
    const indurtdb_meta_t   *meta;
} vsoa_rbe_ctx_t;

static bool vsoa_rbe_decide(void *ctx, rtdbd_session_t *s, uint32_t point_id)
{
    vsoa_rbe_ctx_t *c = (vsoa_rbe_ctx_t *)ctx;
    /* 同源不变量（评审 M-2）：gate 用入参 point_id 判订阅
     * （rtdbd_session_subs_contains），而 rtdbd_dispatch_notify_rbe 内部用
     * c->w->point_id 推进 subtab 的 RBE 状态。接线处 vsoa_notify_dispatch 把
     * 同一个 w 同时作为 gate 的 point_id 来源（w->point_id）与 rctx.w，故二者
     * 必须同源；否则订阅判定与状态推进会对错点，破坏门控语义。
     * 项目无运行时 assert 范式（仅编译期 _Static_assert；引入 <assert.h> 会
     * 破坏本文件的分层边界），改用等价的「不同源返回 false」防御：正确路径
     * （同源）返回值不变，不同源时抑制发送而非继续推进 RBE 状态。 */
    if (c->w->point_id != point_id) {
        return false;
    }
    return rtdbd_dispatch_notify_rbe(&s->sub, c->w, c->meta);
}

/* 变更通知定向投递（§8.3-5）：对每个**订阅了该点**的 per-cli 会话做 RBE 门控，
 * 通过者用 cli_datagram 逐连接定向发送（**禁用** publish / quick_datagram）。
 * role=sync_peer 的会话不得接收 NOTIFY（防回环，§8.3-4/§8.3-5）。
 * 门控 + 计数走 session 层传输无关纯函数，adapter 全局计数由本循环推进。 */
static void vsoa_notify_dispatch(rtdbd_vsoa_adapter_t *a,
                                 const rtdbd_write_req_t *w,
                                 const indurtdb_meta_t *meta)
{
    if (!a || !a->server || !w) {
        return;
    }
    rtdbd_notify_t nt;
    if (rtdbd_dispatch_notify(a->handle, w, &nt) != RTDBD_ST_OK) {
        return;
    }
    vsoa_url_t url;
    memset(&url, 0, sizeof(url));
    static const char notify_url[] = RTDBD_VSOA_URL_PREFIX "/notify";
    url.url = (char *)notify_url;
    url.url_len = sizeof(notify_url) - 1;

    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (!a->cli[i].used) {
            continue; /* ① 空槽 */
        }
        rtdbd_session_t *sess = &a->cli[i].session;
        vsoa_rbe_ctx_t rctx = { w, meta };
        rtdbd_notify_gate_t g = rtdbd_session_notify_gate(
            sess, w->point_id, vsoa_rbe_decide, &rctx, &a->notify_counters);
        if (g != RTDBD_NOTIFY_GATE_HIT) {
            continue; /* ②③ sync_peer / 未订阅 SKIP；④ RBE 抑制 MISS */
        }
        if (!rtdbd_session_notify_deliver_guard(sess, w->point_id,
                                                &a->notify_counters)) {
            continue; /* 发送点护栏：非订阅者/sync_peer 差点收包 → 记泄露并拒发 */
        }
        vsoa_payload_t pl;
        memset(&pl, 0, sizeof(pl));
        pl.data = &nt;
        pl.data_len = sizeof(nt);
        (void)vsoa_server_cli_datagram(a->server, a->cli[i].id, &url, &pl);
    }
}

/* 命令回调：VSOA URL + payload → 归一化 URL → opcode → dispatch → reply。 */
/* ---- v3.12 M3.3 切片 C：/sync/peers/{set,get}（§197）----
 * 限 role=sync_admin + 已握手 + loopback；整组替换与 UDS **共用**
 * rtdbd_peers_apply_set()，故两前端看到同一张表、同一套延迟动作。 */
static bool vsoa_cli_loopback(vsoa_server_t *server, vsoa_cli_id_t id)
{
    struct sockaddr_storage sa;
    socklen_t slen = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (!vsoa_server_cli_address(server, id, (struct sockaddr *)&sa, &slen) ||
        sa.ss_family != AF_INET) {
        return false;
    }
    const struct sockaddr_in *in4 = (const struct sockaddr_in *)&sa;
    return (ntohl(in4->sin_addr.s_addr) & 0xFF000000u) == 0x7F000000u;
}

static uint16_t vsoa_peers_admin(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                                 vsoa_cli_id_t id, const char *bare,
                                 const uint8_t *req, size_t req_len,
                                 size_t *resp_len)
{
    *resp_len = 0;
    rtdbd_session_t *sess = cli_session(a, id);
    if (!sess || !sess->cred.cred_ok) return RTDBD_ST_DENIED; /* 未握手 */
    if (sess->role != RTDBD_ROLE_SYNC_ADMIN) return RTDBD_ST_DENIED;
    if (!vsoa_cli_loopback(server, id)) return RTDBD_ST_DENIED;

    if (strcmp(bare, "/sync/peers/set") == 0) {
        if (req_len != sizeof(rtdbd_peers_resp_t)) return RTDBD_ST_BAD_REQUEST;
        rtdbd_peers_resp_t body;
        memcpy(&body, req, sizeof(body));
        if (body.count > RTDBD_PEER_MAX) return RTDBD_ST_BAD_REQUEST;
        /* 与 UDS op_set_peers 同一函数：原子整组替换、失败旧表不变、diff 入队 */
        if (rtdbd_peers_apply_set(body.peers, body.count) != 0) return RTDBD_ST_BAD_REQUEST;
        return RTDBD_ST_OK;
    }

    rtdbd_peers_resp_t out;
    memset(&out, 0, sizeof(out));
    rtdbd_peer_table_t *tbl = rtdbd_peers_global();
    out.count = tbl->count;
    if (out.count > 0u) {
        memcpy(out.peers, tbl->peers, (size_t)out.count * sizeof(rtdbd_peer_t));
    }
    memcpy(a->resp, &out, sizeof(out));
    *resp_len = sizeof(out);
    return RTDBD_ST_OK;
}

/* ---- S2：同步通道专有 URL handler（4 URL，VSOA RPC call 全同步，不走 quick）---- */
static uint64_t sync_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 本机 node_id：配置优先，回退 service_id（S2 临时，S2b 正式配置）。 */
static const char *sync_local_node_id(rtdbd_vsoa_adapter_t *a, rtdbd_session_t *s)
{
    if (a->sync_node_id[0] != '\0') return a->sync_node_id;
    if (s && s->service_id[0] != '\0') return s->service_id;
    return "local";
}

/* S2 门禁：仅已握手的 sync_peer 且来源在成员表可访问同步 URL（fix P1-5）。
 * 复用 vsoa_hello 的 sync_peer 来源判据（:287-311）。 */
static bool sync_gate_allowed(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                              vsoa_cli_id_t id, rtdbd_session_t **out_s)
{
    rtdbd_session_t *s = cli_session(a, id);
    if (!s || !s->cred.cred_ok || s->role != RTDBD_ROLE_SYNC_PEER) return false;
    struct sockaddr_storage sa;
    socklen_t slen = sizeof(sa);
    memset(&sa, 0, sizeof(sa));
    if (!vsoa_server_cli_address(server, id, (struct sockaddr *)&sa, &slen) ||
        sa.ss_family != AF_INET) return false;
    char src[INET_ADDRSTRLEN] = {0};
    const struct sockaddr_in *in4 = (const struct sockaddr_in *)&sa;
    if (!inet_ntop(AF_INET, &in4->sin_addr, src, sizeof(src))) return false;
    if (rtdbd_authz_sync_peer_source_allowed(rtdbd_peers_global(), src) != RTDBD_ST_OK) {
        return false;
    }
    if (out_s) *out_s = s;
    return true;
}

/* 本机 digest：枚举已注册点 → S1 rtdbd_digest_compute（复用 S1 纯函数）。 */
static void sync_local_digest(rtdbd_vsoa_adapter_t *a, uint64_t *sh, uint64_t *vd,
                              uint32_t *np)
{
    *sh = 0u; *vd = 0u; *np = 0u;
    if (!a->handle) return;
    static indurtdb_point_t pts[512];
    uint32_t n = 0u;
    uint32_t maxp = a->readonly_max_points ? a->readonly_max_points : 512u;
    if (maxp > 512u) maxp = 512u;
    for (uint32_t id = 0; id < maxp && n < 512u; ++id) {
        indurtdb_point_t pt;
        if (indurtdb_h_read_point(a->handle, id, &pt) != 0) continue;
        if (pt.name[0] == '\0') continue;
        pts[n++] = pt;
    }
    rtdbd_digest_t d;
    if (rtdbd_digest_compute(pts, n, &d)) {
        *sh = d.struct_hash; *vd = d.value_digest; *np = d.n_points;
    }
}

/* 登记 WAIT 等待上下文（回调内只登记、立即返回，不阻塞——fix P0-1）。 */
static void sync_wait_register(rtdbd_vsoa_adapter_t *a, vsoa_cli_id_t id,
                               uint32_t seq, uint16_t tun,
                               const rtdbd_sync_wait_req_t *req)
{
    for (uint32_t k = 0; k < RTDBD_SYNC_WAIT_MAX; ++k) {
        rtdbd_sync_wait_ctx_t *w = &a->wait_ctx[k];
        if (!w->used) {
            memset(w, 0, sizeof(*w));
            w->used = true; w->cli_id = id; w->seq = seq; w->tun = tun;
            w->min_epoch = req->min_epoch; w->required_acks = req->required_acks;
            w->deadline_ns = sync_now_ns() +
                (uint64_t)(req->timeout_ms ? req->timeout_ms : RTDBD_SYNC_WAIT_TIMEOUT_MS) * 1000000ull;
            return;
        }
    }
}

/* 4 URL 统一分派：0=hello 1=snapshot 2=delta 3=wait。 */
static void vsoa_sync_handle(rtdbd_vsoa_adapter_t *a, vsoa_server_t *server,
                             vsoa_cli_id_t id, vsoa_header_t *hdr,
                             const char *bare, const uint8_t *req, size_t req_len,
                             int kind)
{
    (void)bare;
    uint32_t seq = hdr ? ntohl(hdr->seqno) : 0u; /* 主机序（终验 P1） */
    uint16_t tun = hdr ? ntohs(hdr->tunid) : 0u;
    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    rtdbd_session_t *s = NULL;
    if (!sync_gate_allowed(a, server, id, &s)) {
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_DENIED), seq, tun, &out);
        return;
    }
    size_t expect = (kind == 0) ? sizeof(rtdbd_sync_hello_req_t)
                  : (kind == 1) ? sizeof(rtdbd_sync_snapshot_req_t)
                  : (kind == 2) ? sizeof(rtdbd_sync_delta_req_t)
                                : sizeof(rtdbd_sync_wait_req_t);
    if (!req || req_len != expect) {
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_BAD_REQUEST), seq, tun, &out);
        return;
    }

    if (kind == 0) { /* /sync/hello */
        rtdbd_sync_hello_req_t rq;
        memcpy(&rq, req, sizeof(rq));
        rtdbd_vsoa_cli_entry_t *ce = cli_entry_for(a, id);
        rtdbd_sync_peer_state_t *ps = ce ? &ce->sync_state : NULL;
        if (!ps) return;
        ps->epoch = rq.epoch; ps->struct_hash = rq.struct_hash;
        ps->value_digest = rq.value_digest; ps->n_points = rq.n_points;
        memcpy(ps->node_id, rq.node_id, sizeof(ps->node_id));
        ps->valid = true;
        rtdbd_sync_hello_resp_t rp;
        memset(&rp, 0, sizeof(rp));
        sync_local_digest(a, &rp.struct_hash, &rp.value_digest, &rp.n_points);
        rp.epoch = rtdbd_epoch_current();
        const char *local_id = sync_local_node_id(a, s);
        {
            size_t nid_len = strlen(local_id);
            if (nid_len >= sizeof(rp.node_id)) nid_len = sizeof(rp.node_id) - 1u;
            memcpy(rp.node_id, local_id, nid_len);
            rp.node_id[nid_len] = '\0';
        }
        rp.accepted = 1u; /* S2 不选全源（S3） */
        memcpy(a->resp, &rp, sizeof(rp));
        out.data = a->resp; out.data_len = sizeof(rp);
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_OK), seq, tun, &out);
        return;
    }
    if (kind == 1) { /* /sync/snapshot：header→param、点位数组→data */
        rtdbd_sync_snapshot_req_t rq;
        memcpy(&rq, req, sizeof(rq));
        uint32_t cap = rtdbd_sync_page_capacity(rq.page_size);
        uint32_t n_all = 0u;
        /* 计算总点数（枚举已注册点；total_pages 漂移留 S3 seqlock） */
        {
            rtdbd_digest_t dummy;
            static indurtdb_point_t pts[512];
            uint32_t n = 0u, maxp = a->readonly_max_points ? a->readonly_max_points : 512u;
            if (maxp > 512u) maxp = 512u;
            for (uint32_t pid = 0; pid < maxp && n < 512u; ++pid) {
                indurtdb_point_t pt;
                if (indurtdb_h_read_point(a->handle, pid, &pt) != 0) continue;
                if (pt.name[0] == '\0') continue;
                pts[n++] = pt;
            }
            (void)rtdbd_digest_compute(pts, n, &dummy);
            n_all = n;
        }
        rtdbd_sync_snapshot_resp_t hp;
        memset(&hp, 0, sizeof(hp));
        hp.page_index = rq.page_index;
        hp.total_pages = rtdbd_sync_page_count(n_all, cap);
        uint32_t start = rq.page_index * cap;
        uint32_t n = (start < n_all && cap > 0u) ? (n_all - start < cap ? n_all - start : cap) : 0u;
        hp.n_points = n;
        out.param = (char *)a->resp; out.param_len = sizeof(hp);
        memcpy(a->resp, &hp, sizeof(hp));
        if (n > 0u) {
            static indurtdb_point_t pts[512];
            uint32_t k = 0u, maxp = a->readonly_max_points ? a->readonly_max_points : 512u;
            if (maxp > 512u) maxp = 512u;
            for (uint32_t pid = 0; pid < maxp && k < 512u; ++pid) {
                indurtdb_point_t pt;
                if (indurtdb_h_read_point(a->handle, pid, &pt) != 0) continue;
                if (pt.name[0] == '\0') continue;
                if (pid < start) continue;
                if (k >= n) break;
                pts[k++] = pt;
            }
            out.data = (uint8_t *)pts; out.data_len = (size_t)n * sizeof(indurtdb_point_t);
        }
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_OK), seq, tun, &out);
        return;
    }
    if (kind == 2) { /* /sync/delta：游标水位（不宣称真实增量） */
        rtdbd_sync_delta_req_t rq;
        memcpy(&rq, req, sizeof(rq));
        static rtdbd_sync_delta_rec_t recs[64];
        uint64_t applied = rq.from_offset;
        bool more = false;
        uint32_t maxp = a->readonly_max_points ? a->readonly_max_points : 512u;
        uint32_t n = rtdbd_sync_delta_scan(a->handle, maxp, rq.from_offset, rq.max_records,
                                           rtdbd_epoch_current(), sync_local_node_id(a, s),
                                           0u, recs, 64u, &applied, &more);
        rtdbd_sync_delta_resp_t rp;
        memset(&rp, 0, sizeof(rp));
        rp.applied_offset = applied; rp.n_records = n; rp.has_more = more ? 1u : 0u;
        out.param = (char *)a->resp; out.param_len = sizeof(rp);
        memcpy(a->resp, &rp, sizeof(rp));
        if (n > 0u) { out.data = (uint8_t *)recs; out.data_len = (size_t)n * sizeof(recs[0]); }
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_OK), seq, tun, &out);
        return;
    }
    /* kind == 3: /sync/wait —— 只登记，不回包（pump 异步回写） */
    rtdbd_sync_wait_req_t rq;
    memcpy(&rq, req, sizeof(rq));
    sync_wait_register(a, id, seq, tun, &rq);
}

static void vsoa_on_cmd(void *arg, vsoa_server_t *server, vsoa_cli_id_t id,
                         vsoa_header_t *hdr, vsoa_url_t *url, vsoa_payload_t *payload)
{
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)arg;
    if (!a || !url || !url->url) {
        return;
    }

    /* URL 非 NUL 结尾（只有 url_len），须本地拷贝后归一化。 */
    char raw[256];
    size_t n = url->url_len;
    if (n >= sizeof(raw)) {
        n = sizeof(raw) - 1;
    }
    memcpy(raw, url->url, n);
    raw[n] = '\0';

    char bare[128];
    if (!rtdbd_normalize_url(raw, bare, sizeof(bare))) {
        return; /* 缓冲不足：无法归一化，直接不回（由客户端超时暴露） */
    }

    const uint8_t *req = NULL;
    size_t req_len = 0;
    if (payload && payload->data && payload->data_len > 0) {
        req = (const uint8_t *)payload->data;
        req_len = payload->data_len;
    }

    /* hello 是 VSOA 层专有 URL（§8.3-4），不属于 rtdbd opcode 表 → 先处理 */
    if (strcmp(bare, "/hello") == 0) {
        uint16_t hst = vsoa_hello(a, server, id, req, req_len);
        vsoa_payload_t hout;
        memset(&hout, 0, sizeof(hout));
        /* h-A：成功响应带 session_id（老客户端忽略 body、零影响） */
        if (hst == RTDBD_ST_OK) {
            rtdbd_session_t *hs = cli_session(a, id);
            if (hs && hs->session_id != 0u) {
                rtdbd_vsoa_hello_resp_t hresp;
                hresp.session_id = hs->session_id;
                memcpy(a->resp, &hresp, sizeof(hresp));
                hout.data = a->resp;
                hout.data_len = sizeof(hresp);
            }
        }
        uint32_t hseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t htun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)hst),
                                    hseq, htun, &hout);
        return;
    }

    /* h-A：/session/rebind 是 VSOA 专有 URL（仿 hello/peers），不进 opcode 表。 */
    if (strcmp(bare, "/session/rebind") == 0) {
        uint16_t rst = vsoa_session_rebind(a, id, req, req_len, 0u);
        vsoa_payload_t rout;
        memset(&rout, 0, sizeof(rout));
        uint32_t rseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t rtun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)rst),
                                    rseq, rtun, &rout);
        return;
    }

    /* /sync/peers/{set,get} 是 VSOA 专有管控 URL（§197），不进镜像 opcode 表。 */
    if (strcmp(bare, "/sync/peers/set") == 0 || strcmp(bare, "/sync/peers/get") == 0) {
        size_t admin_len = 0;
        uint16_t ast = vsoa_peers_admin(a, server, id, bare, req, req_len, &admin_len);
        vsoa_payload_t aout;
        memset(&aout, 0, sizeof(aout));
        if (admin_len > 0 && admin_len <= sizeof(a->resp)) {
            aout.data = a->resp;
            aout.data_len = admin_len;
        }
        uint32_t aseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t atun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)ast),
                                    aseq, atun, &aout);
        return;
    }

    /* h-B：/health 专有免鉴权分支（UDS op_health 只读免鉴权，两前端一致）。
     * 置于 opcode_from_url 之前，避开 :467 的 role→URL 门禁（health 非管控面）。 */
    if (strcmp(bare, "/health") == 0) {
        if (req_len != 0u) { /* 对齐 UDS 空负载校验 */
            uint32_t hseq = hdr ? ntohl(hdr->seqno) : 0u;
            uint16_t htun = hdr ? ntohs(hdr->tunid) : 0u;
            vsoa_payload_t bad;
            memset(&bad, 0, sizeof(bad));
            (void)vsoa_server_cli_reply(server, id,
                (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_BAD_REQUEST), hseq, htun, &bad);
            return;
        }
        int health = a->handle ? rtdbd_self_check_core(a->handle)
                               : RTDBD_HEALTH_UNHEALTHY;
        /* t-E：VSOA 等价连接检查——used 会话 nsubs 越界 或 未用槽残留订阅
         * （对齐 UDS conn_table_check 的空闲槽残留判据，不引入 cred_ok）。 */
        bool conn_table_ok = true;
        uint32_t nc = 0u;
        for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
            rtdbd_session_t *cs = &a->cli[i].session;
            if (a->cli[i].used) {
                nc++;
                if (cs->nsubs > RTDBD_SUB_MAX) conn_table_ok = false;
            } else if (cs->nsubs != 0u) {
                conn_table_ok = false; /* 空闲槽残留订阅 */
            }
        }
        rtdbd_health_counts_t counts;
        memset(&counts, 0, sizeof(counts));
        counts.n_writes         = g_stats.n_writes;
        counts.n_notifies       = g_stats.n_notifies;
        counts.decode_fail      = g_stats.decode_fail;
        counts.write_rejected   = g_stats.write_rejected;
        counts.write_error      = g_stats.write_error;
        counts.notify_drop      = g_stats.notify_drop;
        counts.notify_send_fail = g_stats.notify_send_fail;
        counts.uptime_ns        = g_stats.uptime_ns;
        rtdbd_health_t hout;
        rtdbd_dispatch_health_fill(health, &counts, nc, conn_table_ok, &hout);
        vsoa_payload_t hout_pl;
        memset(&hout_pl, 0, sizeof(hout_pl));
        memcpy(a->resp, &hout, sizeof(hout));
        hout_pl.data = a->resp;
        hout_pl.data_len = sizeof(hout);
        uint32_t hseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t htun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
            (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_OK), hseq, htun, &hout_pl);
        return;
    }

    /* h-C 前置：4 只读 URL 专有分支（置于 opcode_from_url 之前——4 URL 不在 13 表，
     * 放到门禁后会因 :early-return 不可达；β 门禁在 handler 内显式执行）。 */
    if (strcmp(bare, "/point/find_by_name") == 0) {
        vsoa_readonly_handle(a, server, id, hdr, bare, req, req_len, HC_RO_FIND);
        return;
    }
    if (strcmp(bare, "/point/meta") == 0) {
        vsoa_readonly_handle(a, server, id, hdr, bare, req, req_len, HC_RO_META);
        return;
    }
    if (strcmp(bare, "/point/list") == 0) {
        vsoa_readonly_handle(a, server, id, hdr, bare, req, req_len, HC_RO_LIST);
        return;
    }
    if (strcmp(bare, "/logs") == 0) {
        vsoa_readonly_handle(a, server, id, hdr, bare, req, req_len, HC_RO_LOG);
        return;
    }
    /* S2：同步通道 4 专有 URL（不进 13 表，专有区分派；门禁在 handler 内）。 */
    if (strcmp(bare, "/sync/hello") == 0) {
        vsoa_sync_handle(a, server, id, hdr, bare, req, req_len, 0);
        return;
    }
    if (strcmp(bare, "/sync/snapshot") == 0) {
        vsoa_sync_handle(a, server, id, hdr, bare, req, req_len, 1);
        return;
    }
    if (strcmp(bare, "/sync/delta") == 0) {
        vsoa_sync_handle(a, server, id, hdr, bare, req, req_len, 2);
        return;
    }
    if (strcmp(bare, "/sync/wait") == 0) {
        vsoa_sync_handle(a, server, id, hdr, bare, req, req_len, 3);
        return;
    }

    uint16_t opcode = 0;
    if (rtdbd_opcode_from_url(bare, &opcode) != 0) {
        return; /* 非镜像能力：不处理（也不落错误 reply，避免半实现误导） */
    }

    /* §8.3-4 role→URL 授权（M3.x P7）：镜像能力公共分派路径的 deny-by-default。
     * /hello 与 /sync/peers/{set,get} 已在上面按专有 URL 处理，不会走到这里。
     * 仅对「真握手」会话生效：role 由 token 表赋值（非 UNKNOWN）。
     * allow_unauthenticated + default_uid 的本机联调会话 role 仍是 UNKNOWN，
     * 沿用既有 cred_ok 判据，不受本门禁影响（避免误伤既有 e2e 用例）。 */
    rtdbd_session_t *gate_sess = cli_session(a, id);
    if (gate_sess && gate_sess->cred.cred_ok &&
        gate_sess->role != RTDBD_ROLE_UNKNOWN &&
        !rtdbd_authz_url_allowed(gate_sess->role, bare)) {
        vsoa_payload_t denied;
        memset(&denied, 0, sizeof(denied));
        uint32_t dseq = hdr ? ntohl(hdr->seqno) : 0u;
        uint16_t dtun = hdr ? ntohs(hdr->tunid) : 0u;
        (void)vsoa_server_cli_reply(server, id,
                                    (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_DENIED),
                                    dseq, dtun, &denied);
        return;
    }

    size_t resp_len = 0;
    int st = 0;
    if (opcode == RTDBD_OP_CAPS) {
        /* /caps：走**共享**的 transport 无关语义（与 UDS 侧同一函数），
         * 故 VSOA 与 UDS 两前端的 32B 响应体字节一致（§8.3-3 / §7.4）。 */
        if (!a->cfg_set) {
            st = 9; /* RTDBD_ST_NOT_IMPLEMENTED：未配置能力快照，显式未实现 */
        } else {
            st = (int)rtdbd_dispatch_caps(&a->cfg, a->resp, sizeof(a->resp), &resp_len);
        }
    } else if (opcode == RTDBD_OP_GET) {
        /* /point/get：读点值 —— 走**共享**的 transport 无关语义（与 UDS 侧同一
         * 函数），故两前端读同一点位得到字节一致的 64B 响应体。 */
        if (!a->handle) {
            st = 9; /* RTDBD_ST_NOT_IMPLEMENTED：未注入点位库句柄 */
        } else if (req_len != sizeof(rtdbd_get_req_t)) {
            st = 1; /* RTDBD_ST_BAD_REQUEST：请求体长度不符 */
        } else {
            rtdbd_get_req_t greq;
            memcpy(&greq, req, sizeof(greq));
            st = (int)rtdbd_dispatch_get(a->handle, greq.point_id,
                                         a->resp, sizeof(a->resp), &resp_len);
        }
    } else if (opcode == RTDBD_OP_SELECT) {
        /* /select：设置**连接级**默认分区（写入该连接的 session）。
         * 语义与 UDS 侧 op_select 一致：分区非法 → DENIED 且**不改**连接状态。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9; /* 无会话状态（未连接或会话表满） */
        } else if (req_len != sizeof(uint32_t)) {
            st = 1; /* BAD_REQUEST：请求体长度不符 */
        } else {
            uint32_t part = 0;
            memcpy(&part, req, sizeof(part));
            if (a->conf && !rtdbd_conf_partition_allowed(a->conf, part)) {
                st = 2; /* DENIED：不改连接状态 */
            } else {
                sess->partition = part;
                st = 0;
            }
        }
    } else if (opcode == RTDBD_OP_SUBSCRIBE || opcode == RTDBD_OP_UNSUBSCRIBE) {
        /* 订阅 / 退订：会话的订阅集合是**传输无关真源**（session.sub），
         * VSOA 重连后据此重建（§8.3-5）。鉴权走共享语义（点位分区）。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9; /* 无会话状态 */
        } else if (!sess->cred.cred_ok && !a->allow_unauthenticated) {
            st = 2; /* DENIED：未握手（§8.3-4 强制 hello 握手） */
        } else if (req_len != sizeof(rtdbd_sub_req_t)) {
            st = 1; /* BAD_REQUEST */
        } else {
            rtdbd_sub_req_t sreq;
            memcpy(&sreq, req, sizeof(sreq));
            if (opcode == RTDBD_OP_SUBSCRIBE) {
                uint16_t authz = rtdbd_dispatch_subscribe_authz(a->handle, a->policy,
                                                                a->conf, sess->cred.uid,
                                                                sreq.point_id);
                if (authz != RTDBD_ST_OK) {
                    st = authz;
                } else {
                    rtdbd_subs_add_result_t ar = rtdbd_session_subs_add(sess, sreq.point_id,
                                                                        RTDBD_SUB_MAX);
                    st = (ar == RTDBD_SUBS_ADD_FAIL) ? 5 /* NO_MEM */ : 0;
                }
            } else {
                rtdbd_session_subs_remove(sess, sreq.point_id);
                st = 0;
            }
        }
    } else if (opcode == RTDBD_OP_WRITE) {
        /* /point/write：写权威路径。鉴权与落值走**共享**语义
         * （rtdbd_dispatch_write），审计由前端回调完成。
         * ⚠ 变更通知（定向投递）尚未实现 —— 见 14 号计划 M1.8 留白。 */
        rtdbd_session_t *sess = cli_session(a, id);
        if (!sess) {
            st = 9;
        } else if (!sess->cred.cred_ok && !a->allow_unauthenticated) {
            st = 2; /* DENIED：未握手（§8.3-4 强制 hello 握手） */
        } else if (!a->policy || !a->conf || !a->handle) {
            st = 9; /* 未注入写路径依赖：显式未实现，绝不默认放行 */
        } else if (req_len != sizeof(rtdbd_write_req_t)) {
            st = 1; /* BAD_REQUEST */
        } else {
            rtdbd_write_req_t wreq;
            memcpy(&wreq, req, sizeof(wreq));
            indurtdb_meta_t wmeta;
            st = (int)rtdbd_dispatch_write(a->handle, a->policy, a->conf, &wreq,
                                           sess->cred.uid, sess->partition, &wmeta);
            if (st == RTDBD_ST_OK) {
                if (a->audit) {
                    a->audit(a->audit_ctx, sess->cred.pid, sess->cred.uid,
                             sess->partition, wreq.point_id, 0 /* ts_ns 由前端补 */,
                             sess->service_id);
                }
                /* 变更通知：逐连接**定向**投递（§8.3-5，禁用 publish / quick）。
                 * 帧内容与 RBE 门控走共享语义（两端一致），投递机制属传输层。 */
                vsoa_notify_dispatch(a, &wreq, &wmeta);
            }
        }
    } else if (a->handler.handle) {
        st = a->handler.handle(a->handler.ctx, opcode, req, req_len,
                               a->resp, sizeof(a->resp), &resp_len);
    }

    vsoa_payload_t out;
    memset(&out, 0, sizeof(out));
    if (resp_len > 0 && resp_len <= sizeof(a->resp)) {
        out.data = a->resp;
        out.data_len = resp_len;
    }
    /* 字节序：cmd 回调的 vsoa_header_t 多字节成员是**网络字节序**
     * （SDK 头文件明示），而 vsoa_server_cli_reply 的 seqno/tunid 期望**主机
     * 字节序**（SDK 内部再转网络序）。不转换时：同一连接的第 1 个请求
     * （seqno=0）恰好正常，第 2 个起（seqno≠0）客户端匹配不上 → RPC 超时。
     * 该缺陷只有在同一连接连续发第二个 RPC 时才暴露。 */
    uint32_t seqno = hdr ? ntohl(hdr->seqno) : 0u;
    uint16_t tunid = hdr ? ntohs(hdr->tunid) : 0u;
    /* status 走 0x80 + RTDBD_ST_*（§7.2）；uint8_t 足够（0x80..0x89）。 */
    (void)vsoa_server_cli_reply(server, id, (uint8_t)rtdbd_st_to_vsoa_status((uint8_t)st),
                                seqno, tunid, &out);
}

rtdbd_vsoa_adapter_t *rtdbd_vsoa_adapter_create(const rtdbd_vsoa_handler_t *handler)
{
    if (!handler || !handler->handle) {
        return NULL;
    }
    rtdbd_vsoa_adapter_t *a = (rtdbd_vsoa_adapter_t *)calloc(1, sizeof(*a));
    if (!a) {
        return NULL;
    }
    a->handler = *handler;
    a->server = vsoa_server_create("{\"name\":\"indurtdb_rtdbd\",\"n\":1}");
    if (!a->server) {
        free(a);
        return NULL;
    }
    vsoa_server_on_cli(a->server, vsoa_on_cli, a);
    /* hello 是 VSOA 层专有 URL（§8.3-4），不属于 rtdbd 镜像清单，须单独注册
     * listener，否则 SDK 会以「无对应 URL」回 status=3（实测）。 */
    {
        static char hello_full[128];
        int hk = snprintf(hello_full, sizeof(hello_full), "%s/hello",
                          RTDBD_VSOA_URL_PREFIX);
        if (hk > 0 && (size_t)hk < sizeof(hello_full)) {
            vsoa_url_t hu;
            memset(&hu, 0, sizeof(hu));
            hu.url = hello_full;
            hu.url_len = strlen(hello_full);
            if (!vsoa_server_add_listener(a->server, &hu, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server);
                free(a);
                return NULL;
            }
        }
    }
    /* h-A：/session/rebind 专有 URL（订阅快照恢复），仿 hello 单独注册。 */
    {
        static char rebind_full[128];
        int rk = snprintf(rebind_full, sizeof(rebind_full), "%s/session/rebind",
                          RTDBD_VSOA_URL_PREFIX);
        if (rk > 0 && (size_t)rk < sizeof(rebind_full)) {
            vsoa_url_t ru;
            memset(&ru, 0, sizeof(ru));
            ru.url = rebind_full;
            ru.url_len = strlen(rebind_full);
            if (!vsoa_server_add_listener(a->server, &ru, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server);
                free(a);
                return NULL;
            }
        }
    }
    {
        static const char *const admin_urls[] = {"/sync/peers/set", "/sync/peers/get"};
        static char admin_full[2][128];
        for (size_t i = 0; i < sizeof(admin_urls) / sizeof(admin_urls[0]); ++i) {
            int ak = snprintf(admin_full[i], sizeof(admin_full[i]), "%s%s",
                              RTDBD_VSOA_URL_PREFIX, admin_urls[i]);
            if (ak <= 0 || (size_t)ak >= sizeof(admin_full[i])) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
            vsoa_url_t au;
            memset(&au, 0, sizeof(au));
            au.url = admin_full[i];
            au.url_len = strlen(admin_full[i]);
            if (!vsoa_server_add_listener(a->server, &au, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
        }
    }
    for (size_t i = 0; i < VSOA_MIRROR_COUNT; ++i) {
        char full[128];
        int k = snprintf(full, sizeof(full), "%s%s", RTDBD_VSOA_URL_PREFIX,
                         VSOA_MIRROR_URLS[i]);
        if (k <= 0 || (size_t)k >= sizeof(full)) {
            continue;
        }
        vsoa_url_t u;
        memset(&u, 0, sizeof(u));
        u.url = full;
        u.url_len = strlen(full);
        if (!vsoa_server_add_listener(a->server, &u, vsoa_on_cmd, a)) {
            /* 注册失败即视为不可用：宁可不启动，也不做半实现。 */
            vsoa_server_close(a->server);
            free(a);
            return NULL;
        }
    }
    /* h-C 前置：4 只读 URL 单独注册（P0-1：漏注册则 SDK 回 status=3、分支不可达）。
     * 不进 13 镜像表（保持 opcode 冻结）；注册失败即 adapter 创建失败、不半实现。 */
    {
        static const char *const ro_urls[] = {
            "/point/find_by_name", "/point/meta", "/point/list", "/logs"
        };
        for (size_t i = 0; i < sizeof(ro_urls) / sizeof(ro_urls[0]); ++i) {
            char full[128];
            int k = snprintf(full, sizeof(full), "%s%s", RTDBD_VSOA_URL_PREFIX, ro_urls[i]);
            if (k <= 0 || (size_t)k >= sizeof(full)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
            vsoa_url_t u;
            memset(&u, 0, sizeof(u));
            u.url = full;
            u.url_len = strlen(full);
            if (!vsoa_server_add_listener(a->server, &u, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
        }
    }
    /* S2：4 个同步通道专有 URL listener（不进 13 表/opcode 表）。 */
    {
        static const char *const sync_urls[] = {
            "/sync/hello", "/sync/snapshot", "/sync/delta", "/sync/wait"
        };
        for (size_t i = 0; i < sizeof(sync_urls) / sizeof(sync_urls[0]); ++i) {
            char full[128];
            int k = snprintf(full, sizeof(full), "%s%s", RTDBD_VSOA_URL_PREFIX, sync_urls[i]);
            if (k <= 0 || (size_t)k >= sizeof(full)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
            vsoa_url_t u;
            memset(&u, 0, sizeof(u));
            u.url = full;
            u.url_len = strlen(full);
            if (!vsoa_server_add_listener(a->server, &u, vsoa_on_cmd, a)) {
                vsoa_server_close(a->server); free(a); return NULL;
            }
        }
    }
    return a;
}

void rtdbd_vsoa_adapter_destroy(rtdbd_vsoa_adapter_t *a)
{
    if (!a) {
        return;
    }
    if (a->server) {
        vsoa_server_close(a->server);
    }
    free(a);
}

bool rtdbd_vsoa_adapter_start(rtdbd_vsoa_adapter_t *a, uint16_t port, uint16_t *bound_port)
{
    if (!a || !a->server) {
        return false;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); /* 现场主写入口为 VSOA loopback */

    if (!vsoa_server_start(a->server, (const struct sockaddr *)&addr, sizeof(addr))) {
        return false;
    }
    struct sockaddr_in bound;
    socklen_t blen = sizeof(bound);
    memset(&bound, 0, sizeof(bound));
    if (!vsoa_server_address(a->server, (struct sockaddr *)&bound, &blen)) {
        return false;
    }
    a->port = ntohs(bound.sin_port);
    if (bound_port) {
        *bound_port = a->port;
    }
    return true;
}

int rtdbd_vsoa_adapter_fds(rtdbd_vsoa_adapter_t *a, void *rfds)
{
    if (!a || !a->server || !rfds) {
        return -1;
    }
    return vsoa_server_fds(a->server, (fd_set *)rfds);
}

void rtdbd_vsoa_adapter_input(rtdbd_vsoa_adapter_t *a, const void *rfds)
{
    if (!a || !a->server) {
        return;
    }
    vsoa_server_input_fds(a->server, (const fd_set *)rfds);
}

uint16_t rtdbd_vsoa_adapter_port(const rtdbd_vsoa_adapter_t *a)
{
    return a ? a->port : 0u;
}

rtdbd_notify_counters_t rtdbd_vsoa_adapter_notify_counters(const rtdbd_vsoa_adapter_t *a)
{
    rtdbd_notify_counters_t out;
    if (!a) {
        memset(&out, 0, sizeof(out));
        return out;
    }
    rtdbd_notify_counters_snapshot(&a->notify_counters, &out);
    return out;
}

void rtdbd_vsoa_adapter_set_cfg(rtdbd_vsoa_adapter_t *a, const rtdbd_dispatch_cfg_t *cfg)
{
    if (!a || !cfg) {
        return;
    }
    a->cfg = *cfg;
    a->cfg_set = true;
}

void rtdbd_vsoa_adapter_set_handle(rtdbd_vsoa_adapter_t *a, indurtdb_t *h)
{
    if (!a) {
        return;
    }
    a->handle = h;
}

void rtdbd_vsoa_adapter_set_policy(rtdbd_vsoa_adapter_t *a, const irt_policy_t *policy)
{
    if (a) {
        a->policy = policy;
    }
}

void rtdbd_vsoa_adapter_set_conf(rtdbd_vsoa_adapter_t *a, const rtdbd_conf_t *conf)
{
    if (a) {
        a->conf = conf;
    }
}

void rtdbd_vsoa_adapter_set_audit(rtdbd_vsoa_adapter_t *a, rtdbd_vsoa_audit_fn fn, void *ctx)
{
    if (!a) {
        return;
    }
    a->audit = fn;
    a->audit_ctx = ctx;
}

void rtdbd_vsoa_adapter_set_readonly_sources(rtdbd_vsoa_adapter_t *a,
                                             const void *logbuf, uint32_t max_points)
{
    if (!a) return;
    a->readonly_logbuf = (const irt_logbuf_t *)logbuf;
    a->readonly_max_points = max_points;
}

void rtdbd_vsoa_adapter_set_sync_node_id(rtdbd_vsoa_adapter_t *a, const char *node_id)
{
    if (!a || !node_id) return;
    size_t n = strlen(node_id);
    if (n >= sizeof(a->sync_node_id)) n = sizeof(a->sync_node_id) - 1u;
    memcpy(a->sync_node_id, node_id, n);
    a->sync_node_id[n] = '\0';
}

/* S3：投影 cli[].sync_state.valid 为选源候选 + 显式注入本机候选（见 adapter.h）。 */
bool rtdbd_vsoa_adapter_collect_source_candidates(rtdbd_vsoa_adapter_t *a,
                                                  rtdbd_source_candidate_t *out, uint32_t cap,
                                                  const char *local_node_id,
                                                  const rtdbd_digest_t *local_digest)
{
    if (!a || !out || cap == 0u || !local_node_id || !local_digest) return false;
    uint32_t n = 0u;
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI && n < cap; ++i) {
        rtdbd_vsoa_cli_entry_t *e = &a->cli[i];
        if (!e->used || !e->sync_state.valid) continue;
        rtdbd_source_candidate_t *c = &out[n++];
        memset(c, 0, sizeof(*c));
        c->epoch = e->sync_state.epoch;
        c->struct_hash = e->sync_state.struct_hash;
        c->value_digest = e->sync_state.value_digest;
        c->n_points = e->sync_state.n_points;
        memcpy(c->node_id, e->sync_state.node_id, sizeof(c->node_id));
        c->node_id[sizeof(c->node_id) - 1u] = '\0';
        c->boot_id = 0u;   /* S3：boot_id 仅审计、不参与选源（置 0 归一化） */
        c->offset = 0u;    /* 入站 hello 无 offset，去重键由调用方 backlog 补 */
        c->valid = true;
    }
    if (n >= cap) return false; /* 无槽位注入本机 → 拒绝裁决 */
    rtdbd_source_candidate_t *self = &out[n++];
    memset(self, 0, sizeof(*self));
    self->epoch = rtdbd_epoch_current();
    self->struct_hash = local_digest->struct_hash;
    self->value_digest = local_digest->value_digest;
    self->n_points = local_digest->n_points;
    size_t idlen = strlen(local_node_id);
    if (idlen >= sizeof(self->node_id)) idlen = sizeof(self->node_id) - 1u;
    memcpy(self->node_id, local_node_id, idlen);
    self->node_id[idlen] = '\0';
    self->valid = true;
    return true;
}

void rtdbd_vsoa_adapter_wait_pump(rtdbd_vsoa_adapter_t *a)
{
    if (!a) return;
    uint64_t now = sync_now_ns();
    for (uint32_t k = 0; k < RTDBD_SYNC_WAIT_MAX; ++k) {
        rtdbd_sync_wait_ctx_t *w = &a->wait_ctx[k];
        if (!w->used) continue;
        bool expired = (now >= w->deadline_ns);
        /* 收集当前所有有效 peer epoch（断链 cli_id 已清，终验 P1-1）。 */
        uint64_t epochs[RTDBD_VSOA_MAX_CLI];
        uint32_t n = 0u;
        for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
            if (a->cli[i].used && a->cli[i].sync_state.valid) {
                epochs[n++] = a->cli[i].sync_state.epoch;
            }
        }
        uint32_t acks = 0u;
        bool ok = false;
        rtdbd_sync_wait_eval(epochs, n, w->min_epoch, w->required_acks, &acks, &ok);
        if (!expired && !ok) continue; /* 非阻塞：未到期且未满足 */
        /* cli_id 仍有效才回写，防复用串台（终验 P1-1）。 */
        bool live = false;
        for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
            if (a->cli[i].used && a->cli[i].id == w->cli_id) { live = true; break; }
        }
        if (live) {
            rtdbd_sync_wait_resp_t rp;
            memset(&rp, 0, sizeof(rp));
            rp.acks = acks;
            rp.ok = ok ? 1u : 0u; /* 超时不回滚本地写（D5） */
            vsoa_payload_t out;
            memset(&out, 0, sizeof(out));
            memcpy(a->resp, &rp, sizeof(rp));
            out.data = a->resp; out.data_len = sizeof(rp);
            (void)vsoa_server_cli_reply(a->server, w->cli_id,
                (uint8_t)rtdbd_st_to_vsoa_status(RTDBD_ST_OK), w->seq, w->tun, &out);
        }
        memset(w, 0, sizeof(*w));
    }
}

void rtdbd_vsoa_adapter_set_allow_unauthenticated(rtdbd_vsoa_adapter_t *a, bool allow)
{
    if (a) {
        a->allow_unauthenticated = allow;
    }
}

int rtdbd_vsoa_adapter_add_token(rtdbd_vsoa_adapter_t *a, const char *token,
                                  uint32_t uid, uint32_t partition,
                                  const char *service_id, rtdbd_session_role_t role)
{
    if (!a || !token || token[0] == '\0' || !service_id) {
        return -2;
    }
    if (strlen(token) >= sizeof(a->tokens[0].token) ||
        strlen(service_id) >= sizeof(a->tokens[0].service_id)) {
        return -2;
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used && strcmp(a->tokens[i].token, token) == 0) {
            /* 同一 token 重复登记 → 更新该条目（不新增） */
            a->tokens[i].uid = uid;
            a->tokens[i].partition = partition;
            snprintf(a->tokens[i].service_id, sizeof(a->tokens[i].service_id), "%s", service_id);
            a->tokens[i].role = role;
            return 0;
        }
    }
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_TOKEN; ++i) {
        if (a->tokens[i].used) {
            continue;
        }
        a->tokens[i].used = true;
        snprintf(a->tokens[i].token, sizeof(a->tokens[i].token), "%s", token);
        a->tokens[i].uid = uid;
        a->tokens[i].partition = partition;
        snprintf(a->tokens[i].service_id, sizeof(a->tokens[i].service_id), "%s", service_id);
        a->tokens[i].role = role;
        return 0;
    }
    return -1; /* 表满 */
}

void rtdbd_vsoa_adapter_set_default_uid(rtdbd_vsoa_adapter_t *a, uint32_t uid)
{
    if (!a) {
        return;
    }
    a->default_uid = uid;
    /* 已建立连接的会话同步生效（避免连上后改配置不生效） */
    for (uint32_t i = 0; i < RTDBD_VSOA_MAX_CLI; ++i) {
        if (a->cli[i].used) {
            a->cli[i].session.cred.uid = uid;
            a->cli[i].session.cred.cred_ok = (uid != 0);
        }
    }
}
