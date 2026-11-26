/**
 * @file s5_forensics.c
 * @brief S5/S5b 现场取证（AArch64 SylixOS）：四类 client、body 捕获、三模式采集。
 *
 * 用法（S5b）:
 *   s5_forensics <mode> <port> <plc-tok> <plc-svc> <admin-tok> <admin-svc>
 *                <peer-tok> <peer-svc> <self-addr> <out-json> <instance>
 * mode = f2 | f3a | f4-before | f4-after
 * 凭证仅 argv 运行时传入，不入代码/文档。F4 两态由主机侧比对（不依赖板上持久）。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>

#include <rtdbd/protocol.h>
#include "vsoa/vsoa_adapter.h"
#include "rtdbd_notify_latency.h"
#include "cluster/rtdbd_digest.h"
#include "s5_capture.h"

#define S5_DIGEST_MAX_POINTS 256u  /* 取证点集上限（超出则降级 unavailable） */

#define RPC_TIMEOUT_SEC 3
#define S5_BODY_CAP     4224u  /* >= rtdbd_peers_resp_t 4104B */

typedef struct {
    int    done;
    uint8_t status;
    uint8_t body[S5_BODY_CAP];
    size_t  body_len;
} rpc_reply_t;

typedef struct {
    vsoa_client_t *c;
    char token[64];
    char service_id[64];
    int  hello_ok;
} s5_cli_t;

/* vsoa_client.h:98-100：hdr/payload 回调返回即失效，必须回调内 memcpy。 */
static void on_rpc(void *arg, vsoa_client_t *c, vsoa_header_t *h, vsoa_payload_t *p)
{
    rpc_reply_t *r = (rpc_reply_t *)arg;
    (void)c;
    r->done = 1;
    r->status = h ? h->status : 0u;
    if (!p) return;
    if (p->param && p->param_len > 0) {
        size_t n = p->param_len < sizeof(r->body) ? p->param_len : sizeof(r->body);
        memcpy(r->body, p->param, n);
        r->body_len = n;
    } else if (p->data && p->data_len > 0) {
        size_t n = p->data_len < sizeof(r->body) ? p->data_len : sizeof(r->body);
        memcpy(r->body, p->data, n);
        r->body_len = n;
    }
}

static vsoa_client_t *make_client(uint16_t port)
{
    vsoa_client_t *c = vsoa_client_create(NULL, NULL);
    if (!c) return NULL;
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts = { RPC_TIMEOUT_SEC, 0 };
    char info[256] = {0};
    if (!vsoa_client_connect(c, (struct sockaddr *)&addr, sizeof(addr),
                             &ts, NULL, info, sizeof(info))) {
        vsoa_client_close(c);
        return NULL;
    }
    return c;
}

static bool call_rpc(vsoa_client_t *c, const char *url_str, const void *req,
                     size_t len, rpc_reply_t *out)
{
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    u.url = (char *)url_str; u.url_len = strlen(url_str);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = (void *)req; pl.data_len = len;
    rpc_reply_t r; memset(&r, 0, sizeof(r));
    struct timespec ts = { RPC_TIMEOUT_SEC, 0 };
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl,
                          on_rpc, &r, &ts)) return false;
    for (int k = 0; k < 200 && r.done == 0; ++k) {
        fd_set rfds; FD_ZERO(&rfds);
        vsoa_client_fds(c, &rfds);
        struct timeval tv = {0, 10000};
        if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0)
            vsoa_client_input_fds(c, &rfds);
    }
    if (out) *out = r;
    return r.done == 1;
}

static bool hello(vsoa_client_t *c, const s5_cli_t *s)
{
    rtdbd_vsoa_hello_req_t hr;
    memset(&hr, 0, sizeof(hr));
    size_t n1 = strlen(s->token), n2 = strlen(s->service_id);
    if (n1 >= sizeof(hr.token)) n1 = sizeof(hr.token) - 1u;
    if (n2 >= sizeof(hr.service_id)) n2 = sizeof(hr.service_id) - 1u;
    memcpy(hr.token, s->token, n1);
    memcpy(hr.service_id, s->service_id, n2);
    rpc_reply_t r;
    return call_rpc(c, "/indurtdb/v1/hello", &hr, sizeof(hr), &r) && r.status == 0x80;
}

static void mark_unavailable_all(s5_capture_t *c)
{
    static const char *const ua[] = {
        "terminal_hash", "dict_max_snapshot_match", "nodes_converged",
        "machine_restart_pull_full", "partition_duration_s", "stale_epoch_rejected",
        "rbe_gate_hits", "rbe_gate_misses", "snapshot_done_caps_only",
        "deferred_ops", "callback_reconnect_attempts", "caps_reset_ok",
        "t3_wall_ms", "t3_rss_sender_kb", "t3_rss_receiver_kb", "t3_frames", "t3_bytes",
        "join_triggers_full", "deny_non_loopback", "subs_restored",
        "epoch_before", "epoch_after"
    };
    const char *reason = "产品语义不足或无现场出口（S1-S4未覆盖），现场不可得";
    for (size_t i = 0; i < sizeof(ua) / sizeof(ua[0]); ++i)
        (void)s5_field_set_unavailable(c, ua[i], reason);
}

/* 从 4104B peers resp 生成紧凑摘要 "count=N;addr|node,..."。 */
static bool peers_summary(const uint8_t *body, size_t len, char *out, size_t cap)
{
    if (!body || len < sizeof(rtdbd_peers_resp_t) || !out || cap == 0u) return false;
    rtdbd_peers_resp_t resp;
    memcpy(&resp, body, sizeof(resp));
    if (resp.count > RTDBD_PEER_MAX) return false;
    int n = snprintf(out, cap, "count=%u", (unsigned)resp.count);
    if (n <= 0 || (size_t)n >= cap) return false;
    size_t used = (size_t)n;
    for (uint32_t i = 0; i < resp.count && used + 96u < cap; ++i) {
        n = snprintf(out + used, cap - used, ";%s|%s",
                     resp.peers[i].addr, resp.peers[i].node_id);
        if (n <= 0 || (size_t)n >= cap - used) return false;
        used += (size_t)n;
    }
    return true;
}

/* S5c：现场点集采集 + S1 digest 独立重算。
 * 返回 true 且 out_hex/out_n 有效；失败一律 false（调用方写 unavailable）。 */
static bool collect_digest(vsoa_client_t *plc, char *out_hex, size_t hex_cap, uint32_t *out_n)
{
    static indurtdb_point_t pts[S5_DIGEST_MAX_POINTS];
    uint32_t n = 0u;
    uint64_t offset = 0u;
    bool done = false;

    for (uint32_t page = 0u; page < 32u && !done; ++page) {
        rtdbd_list_req_t lr;
        memset(&lr, 0, sizeof(lr));
        lr.max = 107u;              /* 满页（max=0 禁用：语义歧义） */
        lr.offset = (uint32_t)offset;
        lr.partition = RTDBD_PARTITION_ALL;
        rpc_reply_t rr;
        if (!call_rpc(plc, "/indurtdb/v1/point/list", &lr, sizeof(lr), &rr) ||
            rr.status != 0x80 ||
            rr.body_len % sizeof(rtdbd_point_info_t) != 0u) return false;
        uint32_t cnt = (uint32_t)(rr.body_len / sizeof(rtdbd_point_info_t));
        const rtdbd_point_info_t *infos = (const rtdbd_point_info_t *)rr.body;
        if (cnt == 0u) { done = true; break; } /* 空表：合法（n_points=0、弱证据），非失败 */
        for (uint32_t i = 0u; i < cnt; ++i) {
            if (n >= S5_DIGEST_MAX_POINTS) return false; /* 超上限降级，防假值 */
            rtdbd_get_req_t gr;
            memset(&gr, 0, sizeof(gr));
            gr.point_id = infos[i].point_id;
            rpc_reply_t grr;
            if (!call_rpc(plc, "/indurtdb/v1/point/get", &gr, sizeof(gr), &grr) ||
                grr.status != 0x80 || grr.body_len < sizeof(rtdbd_get_resp_t)) return false;
            rtdbd_get_resp_t g;
            memcpy(&g, grr.body, sizeof(g));
            if (g.type != infos[i].type || g.point_id != infos[i].point_id) return false; /* TOCTOU */
            indurtdb_point_t *p = &pts[n++];
            memset(p, 0, sizeof(*p));
            memcpy(p->name, infos[i].name, sizeof(p->name));
            p->partition = infos[i].partition;
            p->access = infos[i].access;
            p->type = g.type;
            p->quality = g.quality;
            if (g.type == RTDBD_TYPE_STRING) {
                memcpy(p->value.str, g.value_str, sizeof(p->value.str));
            } else {
                /* 数值类型：把 64B 位模式写入 union 前 8 字节（S1 按 type 取有效字节）。 */
                memcpy(&p->value, &g.value_bits, sizeof(g.value_bits));
            }
            p->timestamp_ns = g.timestamp_ns;
            p->source_timestamp_ns = g.source_ts_ns; /* S5c P1-2：非 timestamp_ns */
        }
        /* S5c P0-3：offset 是起始 id；续扫 = 上页最后 point_id + 1。 */
        offset = (uint64_t)infos[cnt - 1u].point_id + 1u;
        if (cnt < 107u) done = true; /* 短页到底 */
    }
    if (!done) return false;

    rtdbd_digest_t dig;
    if (!rtdbd_digest_compute(n ? pts : NULL, n, &dig)) return false;
    int k = snprintf(out_hex, hex_cap, "\"%s\"", dig.hex);
    if (k <= 0 || (size_t)k >= hex_cap) return false;
    *out_n = n;
    return true;
}

int main(int argc, char **argv)
{
    if (argc < 12) {
        printf("usage: s5_forensics <f2|f3a|f4-before|f4-after> <port> "
               "<plc-tok> <plc-svc> <admin-tok> <admin-svc> "
               "<peer-tok> <peer-svc> <self-addr> <out.json> <instance>\n");
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    const char *mode = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *out_path = argv[10];

    s5_cli_t plc, admin, peer;
    memset(&plc, 0, sizeof(plc)); memset(&admin, 0, sizeof(admin)); memset(&peer, 0, sizeof(peer));
    strncpy(plc.token, argv[3], sizeof(plc.token) - 1u);
    strncpy(plc.service_id, argv[4], sizeof(plc.service_id) - 1u);
    strncpy(admin.token, argv[5], sizeof(admin.token) - 1u);
    strncpy(admin.service_id, argv[6], sizeof(admin.service_id) - 1u);
    strncpy(peer.token, argv[7], sizeof(peer.token) - 1u);
    strncpy(peer.service_id, argv[8], sizeof(peer.service_id) - 1u);
    const char *self_addr = argv[9];

    plc.c = make_client(port);
    admin.c = make_client(port);
    peer.c = make_client(port);
    if (!plc.c || !admin.c || !peer.c ||
        !hello(plc.c, &plc) || !hello(admin.c, &admin)) {
        printf("[S5] dual-role hello FAIL (plc=%d admin=%d peer=%d)\n",
               plc.c ? 1 : 0, admin.c ? 1 : 0, peer.c ? 1 : 0);
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    printf("[S5] dual-role hello OK (mode=%s)\n", mode);

    /* 真机修正：VALUE_LEN=8192 后 s5_capture_t 约 540KB，栈上分配会溢出
     * SylixOS 任务栈（S5b 首跑无输出即退出）。改静态存储（单进程单次执行）。 */
    static s5_capture_t cap;
    s5_capture_init(&cap);
    strncpy(cap.mode, mode, sizeof(cap.mode) - 1u);
    strncpy(cap.node_id, argv[11], sizeof(cap.node_id) - 1u);
    mark_unavailable_all(&cap);

    /* S4 /stats/notify body → notify_latency_ns（仅 PLC 可读）。 */
    rpc_reply_t stats;
    if (call_rpc(plc.c, "/indurtdb/v1/stats/notify", NULL, 0u, &stats) &&
        stats.status == 0x80 && stats.body_len >= sizeof(rtdbd_notify_latency_stats_t)) {
        rtdbd_notify_latency_stats_t st;
        memcpy(&st, stats.body, sizeof(st));
        if (st.count > 0u)
            (void)s5_field_set_u64(&cap, "notify_latency_ns", st.last_ns, S5_SRC_ON_SITE);
    }

    /* sync_admin /sync/peers/get body → peers 真值（4104B）。 */
    rpc_reply_t peers;
    static char psum[4096];
    if (call_rpc(admin.c, "/indurtdb/v1/sync/peers/get", NULL, 0u, &peers) &&
        peers.status == 0x80 && peers_summary(peers.body, peers.body_len, psum, sizeof(psum))) {
        /* on-site 值须为合法 JSON 片段：字符串值必须带引号（裸串会破坏 JSON）。 */
        static char psum_json[4200];
        (void)snprintf(psum_json, sizeof(psum_json), "\"%s\"", psum);
        (void)s5_field_set_on_site(&cap, "peers", psum_json);
        (void)s5_field_set_status(&cap, "admin_set_peers_ok", peers.status, S5_SRC_ON_SITE);
    }

    if (strcmp(mode, "f2") == 0) {
        /* P7 deny_bad_token：独立错 token client。 */
        s5_cli_t bad; memset(&bad, 0, sizeof(bad));
        strncpy(bad.token, "invalid-token-for-s5", sizeof(bad.token) - 1u);
        strncpy(bad.service_id, argv[4], sizeof(bad.service_id) - 1u);
        bad.c = make_client(port);
        if (bad.c) {
            rtdbd_vsoa_hello_req_t hr; memset(&hr, 0, sizeof(hr));
            {
                size_t n = strlen(bad.token);
                if (n >= sizeof(hr.token)) n = sizeof(hr.token) - 1u;
                memcpy(hr.token, bad.token, n);
                n = strlen(bad.service_id);
                if (n >= sizeof(hr.service_id)) n = sizeof(hr.service_id) - 1u;
                memcpy(hr.service_id, bad.service_id, n);
            }
            rpc_reply_t rr;
            bool got = call_rpc(bad.c, "/indurtdb/v1/hello", &hr, sizeof(hr), &rr);
            if (got && rr.status != 0x80)
                (void)s5_field_set_status(&cap, "deny_bad_token", rr.status, S5_SRC_ON_SITE);
            vsoa_client_close(bad.c);
        }
        /* P8 reject_self：self_addr 必须与 rtdbd --sync-self-addr 同值（现场指令:20）。 */
        rtdbd_peer_t selfp; memset(&selfp, 0, sizeof(selfp));
        strncpy(selfp.addr, self_addr, sizeof(selfp.addr) - 1u);
        strncpy(selfp.node_id, "self-probe", sizeof(selfp.node_id) - 1u);
        rtdbd_peers_resp_t setreq; memset(&setreq, 0, sizeof(setreq));
        setreq.count = 1u;
        memcpy(&setreq.peers[0], &selfp, sizeof(selfp));
        rpc_reply_t sr;
        if (call_rpc(admin.c, "/indurtdb/v1/sync/peers/set", &setreq, sizeof(setreq), &sr) &&
            sr.status != 0x80)
            (void)s5_field_set_status(&cap, "reject_self", sr.status, S5_SRC_ON_SITE);
        /* P8 reject_invalid / reject_over_limit：构造非法地址 / 33 项。 */
        rtdbd_peers_resp_t inv; memset(&inv, 0, sizeof(inv));
        inv.count = 1u; strcpy(inv.peers[0].addr, "not-an-ip");
        strcpy(inv.peers[0].node_id, "bad");
        rpc_reply_t ir;
        if (call_rpc(admin.c, "/indurtdb/v1/sync/peers/set", &inv, sizeof(inv), &ir) &&
            ir.status != 0x80)
            (void)s5_field_set_status(&cap, "reject_invalid", ir.status, S5_SRC_ON_SITE);
        rtdbd_peers_resp_t over; memset(&over, 0, sizeof(over));
        over.count = RTDBD_PEER_MAX + 1u; /* 字段是 uint32，超出上限 */
        rpc_reply_t orp;
        if (call_rpc(admin.c, "/indurtdb/v1/sync/peers/set", &over, sizeof(over), &orp) &&
            orp.status != 0x80)
            (void)s5_field_set_status(&cap, "reject_over_limit", orp.status, S5_SRC_ON_SITE);
        /* 正向写 1 点（供 peer_notify_leak 负向窗；全授权）。 */
        rtdbd_write_req_t w; memset(&w, 0, sizeof(w));
        w.point_id = 5u; w.type = RTDBD_TYPE_INT32;
        int32_t v = 1234; memcpy(&w.value_bits, &v, sizeof(v));
        rpc_reply_t wr;
        if (call_rpc(plc.c, "/indurtdb/v1/point/write", &w, sizeof(w), &wr) && wr.status == 0x80)
            (void)s5_field_set_u64(&cap, "writes", 1u, S5_SRC_ON_SITE);
        /* deny_peer_write：sync_peer 会话写应 DENIED（若成员表校验通过）。 */
        if (hello(peer.c, &peer)) {
            rpc_reply_t pr;
            if (call_rpc(peer.c, "/indurtdb/v1/point/write", &w, sizeof(w), &pr) && pr.status != 0x80)
                (void)s5_field_set_status(&cap, "deny_peer_write", pr.status, S5_SRC_ON_SITE);
        }
    } else if (strcmp(mode, "f3a") == 0) {
        /* 注入字段保持 unavailable（mark_unavailable_all 已标）。 */
    } else if (strcmp(mode, "f4-before") == 0 || strcmp(mode, "f4-after") == 0) {
        /* S5c：现场点集 → S1 digest 独立重算；失败一律 unavailable+reason（不写假值）。 */
        static char dig_hex[160];
        uint32_t dig_n = 0u;
        if (collect_digest(plc.c, dig_hex, sizeof(dig_hex), &dig_n)) {
            (void)s5_field_set_on_site(&cap, "digest", dig_hex);
            (void)s5_field_set_u64(&cap, "digest_n_points", (uint64_t)dig_n, S5_SRC_ON_SITE);
        } else {
            (void)s5_field_set_unavailable(&cap, "digest",
                "现场点集采集失败（list/get/分页任一异常），不伪造摘要");
            (void)s5_field_set_unavailable(&cap, "digest_n_points",
                "随 digest 一起降级：点集采集失败");
        }
    } else {
        printf("[S5] unknown mode: %s\n", mode);
        printf("S5_RESULT: FAIL\n");
        return 1;
    }

    static char buf[65536];
    if (!s5_capture_validate(&cap) || !s5_capture_emit_json(&cap, buf, sizeof(buf))) {
        printf("[S5] capture validate/emit FAIL\n");
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    FILE *fp = fopen(out_path, "w");
    if (!fp) { printf("[S5] fopen FAIL\nS5_RESULT: FAIL\n"); return 1; }
    fputs(buf, fp); fclose(fp);
    printf("[S5-%s] wrote %s (%u fields)\n", mode, out_path, cap.n_fields);
    printf("S5_RESULT: PASS\n");

    vsoa_client_close(plc.c); vsoa_client_close(admin.c); vsoa_client_close(peer.c);
    return 0;
}
