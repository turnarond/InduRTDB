/**
 * @file sylixos_vsoa_e2e.c
 * @brief SylixOS G2 端到端 harness：VSOA loopback 写入 + 定向 NOTIFY（非 ctest 交付证据）
 * @version 3.12.0
 * @date 2026-11-20
 *
 * 拓扑（单进程内 3 个 vsoa_client，共享 select 事件循环；rtdbd 在独立进程先启动）：
 *   - writer   : hello(plc token) → /point/write 写点位 5（触发 NOTIFY）
 *   - sub      : hello(plc token) → subscribe /notify → 订阅点位 5 → 应收 NOTIFY
 *   - nonsub   : hello(plc token) → 不订阅 → 应**不收** NOTIFY（定向门控对照）
 *
 * 用法：sylixos_vsoa_e2e <vsoa-port> <token> <service_id>
 * 每类断言以 "[E2E n] PASS/FAIL" 打印；全部通过打印 "E2E_RESULT: PASS" 并 exit 0。
 * SylixOS shell 无重定向/&&，输出直接到终端，由 telnet 回显取证；断言+退出码才是证据。
 */

#include <rtdbd/protocol.h>
#include "vsoa_client.h"
#include "vsoa_parser.h"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define RTDBD_VSOA_URL_PREFIX "/indurtdb/v1"
#define POINT_ID 5u
#define E2E_WRITE_VALUE 777

static int g_fail = 0;

static void e2e_result(int idx, int ok, const char* detail) {
    printf("[E2E %d] %s%s%s\n", idx, ok ? "PASS" : "FAIL",
           detail ? " : " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* ---- 客户端状态 ---- */
typedef struct {
    vsoa_client_t* c;
    int subscribed;
    int notify_received;
    uint8_t notify_status;
    uint8_t rpc_done;
    uint8_t rpc_status;
} cli_state_t;

/* datagram 回调：收到定向 NOTIFY 记录 */
static void on_datagram(void* arg, vsoa_client_t* client,
                        vsoa_url_t* url, vsoa_payload_t* payload, bool quick) {
    cli_state_t* s = (cli_state_t*)arg;
    (void)client; (void)url; (void)quick;
    if (payload && payload->data_len >= sizeof(uint32_t)) {
        s->notify_received = 1;
        s->notify_status = 0x80;
    }
}

/* RPC 回调：记录 status */
static void on_rpc(void* arg, vsoa_client_t* client,
                   vsoa_header_t* hdr, vsoa_payload_t* payload) {
    cli_state_t* s = (cli_state_t*)arg;
    (void)client; (void)payload;
    s->rpc_done = 1;
    if (hdr) s->rpc_status = (uint8_t)hdr->status;
}

static cli_state_t* make_client(uint16_t port, const char* token,
                                const char* service_id, int want_sub) {
    cli_state_t* s = (cli_state_t*)calloc(1, sizeof(cli_state_t));
    if (!s) return NULL;
    s->c = vsoa_client_create(NULL, NULL);
    if (!s->c) { free(s); return NULL; }
    vsoa_client_on_datagram(s->c, on_datagram, s);

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts = { 2, 0 };
    char info[256] = {0};
    if (!vsoa_client_connect(s->c, (struct sockaddr*)&addr, sizeof(addr),
                             &ts, NULL, info, sizeof(info))) {
        vsoa_client_close(s->c); free(s); return NULL;
    }

    /* hello 握手（定长 POD） */
    char req[128];
    memset(req, 0, sizeof(req));
    snprintf(req, 64, "%s", token);
    snprintf(req + 64, 64, "%s", service_id);
    vsoa_url_t url; memset(&url, 0, sizeof(url));
    url.url = RTDBD_VSOA_URL_PREFIX "/hello";
    url.url_len = (size_t)strlen(url.url);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = req; pl.data_len = sizeof(req);
    struct timespec cts = { 3, 0 };
    s->rpc_done = 0;
    if (!vsoa_client_call(s->c, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                          on_rpc, s, &cts)) { vsoa_client_close(s->c); free(s); return NULL; }
    /* 驱动事件循环直到 hello 回调 */
    for (int i = 0; i < 300 && !s->rpc_done; ++i) {
        fd_set rfds; FD_ZERO(&rfds);
        vsoa_client_fds(s->c, &rfds);
        struct timeval tv = { 0, 10000 };
        if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0)
            vsoa_client_input_fds(s->c, &rfds);
    }
    if (!s->rpc_done || s->rpc_status != 0x80) { vsoa_client_close(s->c); free(s); return NULL; }

    if (want_sub) {
        /* 订阅 NOTIFY URL（subscribe 是订阅该 URL 的 datagram） */
        url.url = RTDBD_VSOA_URL_PREFIX "/notify";
        url.url_len = (size_t)strlen(url.url);
        if (!vsoa_client_subscribe(s->c, &url, NULL, NULL, &cts)) {
            vsoa_client_close(s->c); free(s); return NULL;
        }
        /* 订阅点位 5（定长 sub_req） */
        uint32_t sub_req = POINT_ID;
        url.url = RTDBD_VSOA_URL_PREFIX "/subscription/add";
        url.url_len = (size_t)strlen(url.url);
        memset(&pl, 0, sizeof(pl));
        pl.data = &sub_req; pl.data_len = sizeof(sub_req);
        s->rpc_done = 0;
        if (!vsoa_client_call(s->c, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                              on_rpc, s, &cts)) { vsoa_client_close(s->c); free(s); return NULL; }
        for (int i = 0; i < 300 && !s->rpc_done; ++i) {
            fd_set rfds; FD_ZERO(&rfds);
            vsoa_client_fds(s->c, &rfds);
            struct timeval tv = { 0, 10000 };
            if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0)
                vsoa_client_input_fds(s->c, &rfds);
        }
        s->subscribed = s->rpc_done && (s->rpc_status == 0x80);
    }
    return s;
}

/* 驱动三个 client 的事件循环若干轮，收 NOTIFY datagram */
static void drive_all(cli_state_t* a, cli_state_t* b, cli_state_t* c, int rounds) {
    for (int i = 0; i < rounds; ++i) {
        fd_set rfds; FD_ZERO(&rfds);
        cli_state_t* arr[3] = { a, b, c };
        for (int k = 0; k < 3; ++k) {
            if (!arr[k] || !arr[k]->c) continue;
            vsoa_client_fds(arr[k]->c, &rfds);
        }
        struct timeval tv = { 0, 10000 };
        int r = select(FD_SETSIZE, &rfds, NULL, NULL, &tv);
        if (r > 0) {
            for (int k = 0; k < 3; ++k) {
                if (arr[k] && arr[k]->c) vsoa_client_input_fds(arr[k]->c, &rfds);
            }
        }
    }
}

int main(int argc, char** argv) {
    if (argc < 4) {
        printf("usage: sylixos_vsoa_e2e <vsoa-port> <token> <service_id>\n");
        printf("E2E_RESULT: FAIL\n");
        return 1;
    }
    uint16_t port = (uint16_t)atoi(argv[1]);
    const char* token = argv[2];
    const char* service_id = argv[3];

    printf("=== InduRTDB SylixOS VSOA E2E (port=%u) ===\n", (unsigned)port);

    cli_state_t* writer = make_client(port, token, service_id, 0);
    cli_state_t* sub = make_client(port, token, service_id, 1);
    cli_state_t* nonsub = make_client(port, token, service_id, 0);

    e2e_result(1, writer && sub && nonsub, "3 clients hello + subscribe established");
    if (!writer || !sub || !nonsub) { printf("E2E_RESULT: FAIL\n"); return 1; }

    /* E2E 2: 订阅者点位订阅成功 */
    e2e_result(2, sub->subscribed, "subscriber subscribed to point 5");

    /* 写入点位 5（触发定向 NOTIFY） */
    rtdbd_write_req_t w;
    memset(&w, 0, sizeof(w));
    w.point_id = POINT_ID;
    w.type = RTDBD_TYPE_INT32;
    int32_t val = E2E_WRITE_VALUE;
    memcpy(&w.value_bits, &val, sizeof(val));
    vsoa_url_t url; memset(&url, 0, sizeof(url));
    url.url = RTDBD_VSOA_URL_PREFIX "/point/write";
    url.url_len = (size_t)strlen(url.url);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    pl.data = &w; pl.data_len = sizeof(w);
    struct timespec cts = { 3, 0 };
    writer->rpc_done = 0;
    int wr_ok = vsoa_client_call(writer->c, VSOA_CLIENT_RPC_METHOD_GET, &url, &pl,
                                 on_rpc, writer, &cts);
    drive_all(writer, sub, nonsub, 50);
    e2e_result(3, wr_ok && writer->rpc_done && writer->rpc_status == 0x80,
               "point/write RPC accepted");

    /* E2E 4: 订阅者收到 NOTIFY */
    drive_all(writer, sub, nonsub, 100);
    e2e_result(4, sub->notify_received, "subscriber received directed NOTIFY");

    /* E2E 5: 非订阅者未收 NOTIFY（定向门控对照） */
    e2e_result(5, !nonsub->notify_received, "non-subscriber did NOT receive NOTIFY");

    vsoa_client_close(writer->c); vsoa_client_close(sub->c); vsoa_client_close(nonsub->c);
    free(writer); free(sub); free(nonsub);

    if (g_fail == 0) { printf("E2E_RESULT: PASS (5/5)\n"); return 0; }
    printf("E2E_RESULT: FAIL (%d failed)\n", g_fail);
    return 1;
}
