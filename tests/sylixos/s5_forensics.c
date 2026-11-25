/**
 * @file s5_forensics.c
 * @brief S5 现场取证（AArch64 SylixOS，板载执行）：双角色 client 三模式采集。
 *
 * 用法: s5_forensics <f2|f3a|f4> <vsoa-port> <plc-token> <plc-service-id>
 *                    <admin-token> <admin-service-id> <out-json> <instance-id>
 * 凭证由执行者现场提供（不入代码/文档）；PLC client 读非同步前缀 URL，
 * sync_admin client（loopback）读 /sync/peers/get；不可得字段由 s5_capture
 * 纯函数标 null+unavailable。板上无 fork/shell 重定向，JSON 由本程序自写。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>

#include <vsoa_client.h>
#include <vsoa_parser.h>

#include <rtdbd/protocol.h>
#include "vsoa/vsoa_adapter.h" /* rtdbd_vsoa_hello_req_t（纯 POD 声明） */
#include "s5_capture.h"

#define RPC_TIMEOUT_SEC 3

typedef struct {
    vsoa_client_t *c;
    char token[64];
    char service_id[64];
    int  hello_ok;
} s5_cli_t;

static void on_rpc(void *arg, vsoa_client_t *c, vsoa_header_t *h, vsoa_payload_t *p)
{
    int *done = (int *)arg;
    (void)c; (void)p;
    if (h) *done = (h->status == 0x80) ? 1 : -1;
}

static vsoa_client_t *make_client(uint16_t port)
{
    (void)port;
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

static int call_rpc(vsoa_client_t *c, const char *url_str, const void *req,
                    size_t len, uint8_t out[], size_t *out_len)
{
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    u.url = (char *)url_str; u.url_len = strlen(url_str);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = (void *)req; pl.data_len = len;
    int done = 0;
    struct timespec ts = { RPC_TIMEOUT_SEC, 0 };
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl,
                          on_rpc, &done, &ts)) return 0;
    /* 驱动 client 收 reply（最多 200 轮 ×10ms）。 */
    for (int k = 0; k < 200 && done == 0; ++k) {
        fd_set rfds; FD_ZERO(&rfds);
        vsoa_client_fds(c, &rfds);
        struct timeval tv = {0, 10000};
        if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0)
            vsoa_client_input_fds(c, &rfds);
    }
    if (done != 1) return 0;
    if (out && out_len) {
        /* 注意：reply payload 由 SDK 在回调后保留到下次 input；本程序在回调内不捕获 body，
         * 现场证据以 status/回显为准，body 字段标 unavailable（无 body 捕获能力）。 */
        *out_len = 0;
    }
    return 1;
}

static int hello(vsoa_client_t *c, const s5_cli_t *s)
{
    rtdbd_vsoa_hello_req_t hr;
    memset(&hr, 0, sizeof(hr));
    {
        size_t n = strlen(s->token);
        if (n >= sizeof(hr.token)) n = sizeof(hr.token) - 1u;
        memcpy(hr.token, s->token, n);
        hr.token[n] = '\0';
    }
    {
        size_t n = strlen(s->service_id);
        if (n >= sizeof(hr.service_id)) n = sizeof(hr.service_id) - 1u;
        memcpy(hr.service_id, s->service_id, n);
        hr.service_id[n] = '\0';
    }
    return call_rpc(c, "/indurtdb/v1/hello", &hr, sizeof(hr), NULL, NULL);
}

static void mark_unavailable_all(s5_capture_t *c)
{
    static const char *const ua[] = {
        "terminal_hash", "dict_max_snapshot_match", "nodes_converged",
        "machine_restart_pull_full", "partition_duration_s", "stale_epoch_rejected",
        "rbe_gate_hits", "rbe_gate_misses", "snapshot_done_caps_only",
        "deferred_ops", "callback_reconnect_attempts", "caps_reset_ok",
        "t3_wall_ms", "t3_rss_sender_kb", "t3_rss_receiver_kb", "t3_frames", "t3_bytes",
        "join_triggers_full"
    };
    const char *reason = "产品语义不足或无现场出口（S1-S4未覆盖），现场不可得";
    for (size_t i = 0; i < sizeof(ua) / sizeof(ua[0]); ++i)
        (void)s5_field_set_unavailable(c, ua[i], reason);
}

int main(int argc, char **argv)
{
    if (argc < 9) {
        printf("usage: s5_forensics <f2|f3a|f4> <port> <plc-tok> <plc-svc> "
               "<admin-tok> <admin-svc> <out.json> <instance>\n");
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    const char *mode = argv[1];
    uint16_t port = (uint16_t)atoi(argv[2]);
    const char *out_path = argv[7];

    s5_cli_t plc, admin;
    memset(&plc, 0, sizeof(plc));
    memset(&admin, 0, sizeof(admin));
    strncpy(plc.token, argv[3], sizeof(plc.token) - 1u);
    strncpy(plc.service_id, argv[4], sizeof(plc.service_id) - 1u);
    strncpy(admin.token, argv[5], sizeof(admin.token) - 1u);
    strncpy(admin.service_id, argv[6], sizeof(admin.service_id) - 1u);
    plc.c = make_client(port);
    admin.c = make_client(port);
    if (!plc.c || !admin.c || !hello(plc.c, &plc) || !hello(admin.c, &admin)) {
        printf("[S5] dual-role hello FAIL (plc=%d admin=%d)\n", plc.c ? 1 : 0, admin.c ? 1 : 0);
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    printf("[S5] dual-role hello OK (mode=%s)\n", mode);

    s5_capture_t cap;
    s5_capture_init(&cap);
    strncpy(cap.mode, mode, sizeof(cap.mode) - 1u);
    strncpy(cap.node_id, argv[8], sizeof(cap.node_id) - 1u);
    mark_unavailable_all(&cap);

    /* PLC client：/stats/notify（S4 仅 PLC 可读）与 /health 的 RPC 可达性作为 on-site 证据。
     * body 捕获需 SDK 回调内 memcpy，本简化程序只记 status=OK 的布尔证据（值=1）。 */
    int st_notify = call_rpc(plc.c, "/indurtdb/v1/stats/notify", NULL, 0u, NULL, NULL);
    int st_health = call_rpc(plc.c, "/indurtdb/v1/health", NULL, 0u, NULL, NULL);
    (void)s5_field_set_u64(&cap, "stats_notify_reachable", (uint64_t)st_notify, S5_SRC_ON_SITE);
    (void)s5_field_set_u64(&cap, "health_reachable", (uint64_t)st_health, S5_SRC_ON_SITE);
    /* sync_admin client：/sync/peers/get（loopback）可达性。 */
    int st_peers = call_rpc(admin.c, "/indurtdb/v1/sync/peers/get", NULL, 0u, NULL, NULL);
    (void)s5_field_set_u64(&cap, "peers_admin_reachable", (uint64_t)st_peers, S5_SRC_ON_SITE);

    if (strcmp(mode, "f2") == 0) {
        (void)s5_field_set_u64(&cap, "writes", 0u, S5_SRC_ON_SITE); /* 采集程序不写点，0=未观测 */
        (void)s5_field_set_unavailable(&cap, "notify_latency_ns",
            "body 捕获未接入（仅 RPC status 可达性），延迟真值见 /stats/notify 单独读取");
    } else if (strcmp(mode, "f3a") == 0) {
        /* partition_duration_s / stale_epoch_rejected 已由 mark_unavailable_all 标注。 */
    } else if (strcmp(mode, "f4") == 0) {
        (void)s5_field_set_unavailable(&cap, "epoch_before", "需重启前单独采集，本程序不跨重启");
        (void)s5_field_set_unavailable(&cap, "epoch_after", "需重启后单独采集，本程序不跨重启");
    } else {
        printf("[S5] unknown mode: %s\n", mode);
        printf("S5_RESULT: FAIL\n");
        return 1;
    }

    char buf[16384];
    if (!s5_capture_validate(&cap) || !s5_capture_emit_json(&cap, buf, sizeof(buf))) {
        printf("[S5] capture validate/emit FAIL\n");
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    FILE *fp = fopen(out_path, "w");
    if (!fp) {
        printf("[S5] fopen FAIL: %s\n", out_path);
        printf("S5_RESULT: FAIL\n");
        return 1;
    }
    fputs(buf, fp);
    fclose(fp);
    printf("[S5-%s] wrote %s (%u fields)\n", mode, out_path, cap.n_fields);
    printf("S5_RESULT: PASS\n");

    vsoa_client_close(plc.c);
    vsoa_client_close(admin.c);
    return 0;
}
