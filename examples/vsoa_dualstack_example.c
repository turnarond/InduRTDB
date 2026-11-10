/**
 * @file vsoa_dualstack_example.c
 * @brief v3.12 M3.3 方案 A：UDS / VSOA 双栈可运行示例（ON-only）。
 *
 * 自包含：自己 fork/exec 真实 rtdbd（--vsoa-port 0 --vsoa-port-file），
 * 从端口文件读出实际绑定端口，然后交替用 UDS OP_PING 与 VSOA /ping 打流，
 * 验证两栈在**同一进程同一 poll 循环**中同时可达（§8.3-3 题 3）。
 *
 * 构建：cmake -DINDURTDB_ENABLE_VSOA=ON -DVSOA_SDK_ROOT=<libvsoa 树根>
 *       cmake --build build --target vsoa_dualstack_example
 *       ./build/examples/vsoa_dualstack_example
 */
#include <rtdbd/protocol.h>
#include <vsoa_client.h>
#include <vsoa_parser.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef RTDBD_BIN
#define RTDBD_BIN "../build/rtdbd/rtdbd"
#endif

static int write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "w");
    if (!f) return -1;
    int rc = fputs(text, f) < 0 ? -1 : 0;
    fclose(f);
    return rc;
}

static int uds_ping(const char *sock)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return 0;
    struct sockaddr_un a;
    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    size_t sn = strlen(sock);
    if (sn >= sizeof(a.sun_path)) { close(fd); return 0; }
    memcpy(a.sun_path, sock, sn + 1u);
    int ok = 0;
    if (connect(fd, (struct sockaddr *)&a, sizeof(a)) == 0) {
        rtdbd_req_hdr_t req;
        memset(&req, 0, sizeof(req));
        req.magic = RTDBD_MAGIC; req.version = RTDBD_PROTO_VERSION;
        req.opcode = RTDBD_OP_PING; req.payload_len = 0;
        rtdbd_resp_hdr_t resp;
        memset(&resp, 0, sizeof(resp));
        if (write(fd, &req, sizeof(req)) == (ssize_t)sizeof(req) &&
            read(fd, &resp, sizeof(resp)) == (ssize_t)sizeof(resp) &&
            resp.status == RTDBD_ST_OK) ok = 1;
    }
    close(fd);
    return ok;
}

struct RpcResult {
    int replied;
    unsigned char status;
    size_t data_len;
    unsigned char data[8192];   /* 容纳 /sync/peers/get 的 4104B 定长响应 */
};
static void on_rpc(void *arg, vsoa_client_t *c, vsoa_header_t *h, vsoa_payload_t *p)
{
    (void)c;
    struct RpcResult *r = (struct RpcResult *)arg;
    r->replied = 1;
    if (h) r->status = (unsigned char)h->status;
    if (p && p->data && p->data_len > 0) {
        r->data_len = p->data_len < sizeof(r->data) ? p->data_len : sizeof(r->data);
        memcpy(r->data, p->data, r->data_len);
    }
}

/* 与 adapter.h 的 rtdbd_vsoa_hello_req_t 同构（128B）；示例刻意不引内部头，
 * 便于外部集成方直接复制。 */
struct HelloReq { char token[64]; char service_id[64]; };

static int rpc_once(vsoa_client_t *c, const char *url_str,
                    const void *req, size_t req_len, struct RpcResult *r)
{
    vsoa_url_t u; memset(&u, 0, sizeof(u));
    u.url = (char *)url_str; u.url_len = strlen(url_str);
    vsoa_payload_t pl; memset(&pl, 0, sizeof(pl));
    if (req && req_len) {
        pl.data = (char *)req;
        pl.data_len = req_len;
    }
    struct timespec ts; ts.tv_sec = 3; ts.tv_nsec = 0;
    memset(r, 0, sizeof(*r));
    if (!vsoa_client_call(c, VSOA_CLIENT_RPC_METHOD_GET, &u, &pl, on_rpc, r, &ts)) return 0;
    for (int e = 0; e < 2000 && !r->replied; e += 10) {
        fd_set rfds; FD_ZERO(&rfds);
        (void)vsoa_client_fds(c, &rfds);
        struct timeval tv; tv.tv_sec = 0; tv.tv_usec = 10000;
        if (select(FD_SETSIZE, &rfds, NULL, NULL, &tv) > 0) vsoa_client_input_fds(c, &rfds);
    }
    return r->replied;
}

static int vsoa_ping(uint16_t port)
{
    vsoa_client_t *c = vsoa_client_create(NULL, NULL);
    if (!c) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
    char info[256] = {0};
    int ok = 0;
    if (vsoa_client_connect(c, (struct sockaddr *)&a, sizeof(a), &ts,
                            NULL, info, sizeof(info))) {
        struct RpcResult r;
        ok = rpc_once(c, "/indurtdb/v1/ping", NULL, 0, &r) && r.status == 0x80;
    }
    vsoa_client_close(c);
    return ok;
}

/* v3.12 M3.3 切片 C：sync_admin 握手 + VSOA 成员管控只读演示。 */
static int admin_demo(uint16_t port)
{
    vsoa_client_t *c = vsoa_client_create(NULL, NULL);
    if (!c) return 0;
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    struct timespec ts; ts.tv_sec = 2; ts.tv_nsec = 0;
    char info[256] = {0};
    int ok = 0;
    if (vsoa_client_connect(c, (struct sockaddr *)&a, sizeof(a), &ts,
                            NULL, info, sizeof(info))) {
        struct HelloReq h;
        memset(&h, 0, sizeof(h));
        strcpy(h.token, "tok-admin-1");
        strcpy(h.service_id, "dualstack-example");
        struct RpcResult r1;
        int hello_ok = rpc_once(c, "/indurtdb/v1/hello", &h, sizeof(h), &r1) &&
                       r1.status == 0x80;
        struct RpcResult r2;
        int get_ok = rpc_once(c, "/indurtdb/v1/sync/peers/get", NULL, 0, &r2) &&
                     r2.status == 0x80 && r2.data_len == sizeof(rtdbd_peers_resp_t);
        uint32_t count = 0;
        if (get_ok) memcpy(&count, r2.data, sizeof(count));
        printf("[3] VSOA admin hello=%s  /sync/peers/get=%s (count=%u)\n",
               hello_ok ? "OK" : "FAIL", get_ok ? "OK" : "FAIL", count);
        ok = hello_ok && get_ok;
    }
    vsoa_client_close(c);
    return ok;
}

int main(void)
{
    printf("=== InduRTDB UDS/VSOA 双栈示例 (v3.12 M3.3 方案 A) ===\n");
    char dir[] = "/tmp/indurtdb_dualstack_XXXXXX";
    if (!mkdtemp(dir)) { fprintf(stderr, "mkdtemp 失败\n"); return 1; }
    char sock[256], cfg[256], pol[256], pf[256], tk[256], policy[128];
    snprintf(sock, sizeof(sock), "%s/rtdbd.sock", dir);
    snprintf(cfg, sizeof(cfg), "%s/cfg.yaml", dir);
    snprintf(pol, sizeof(pol), "%s/policy.txt", dir);
    snprintf(pf, sizeof(pf), "%s/vsoa.port", dir);
    snprintf(tk, sizeof(tk), "%s/tokens.txt", dir);
    snprintf(policy, sizeof(policy), "%u:*:0:999\n", (unsigned)getuid());
    if (write_text(cfg, "points: []\n") || write_text(pol, policy) ||
        write_text(tk, "tok-admin-1 0 0 dualstack-example sync_admin\n")) return 1;

    pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd", "--socket", sock,
              "--instance", "dualstack_example", "--config", cfg, "--policy", pol,
              "--max-points", "64", "--max-subs", "4",
              "--vsoa-port", "0", "--vsoa-port-file", pf,
              "--vsoa-token-file", tk, (char *)NULL);
        _exit(127);
    }

    uint16_t port = 0;
    int ready = 0;
    for (int i = 0; i < 300; ++i) {
        struct stat st;
        if (stat(sock, &st) != 0) { usleep(10000); continue; }
        FILE *f = fopen(pf, "r");
        if (f) { unsigned v = 0; if (fscanf(f, "%u", &v) == 1) port = (uint16_t)v; fclose(f); }
        if (port != 0) { ready = 1; break; }
        usleep(10000);
    }
    if (!ready) {
        fprintf(stderr, "rtdbd 双栈未就绪（%s；需 ON 构建）\n", RTDBD_BIN);
        kill(pid, SIGTERM); waitpid(pid, NULL, 0);
        return 1;
    }
    printf("[1] rtdbd 起双栈：UDS %s / VSOA 127.0.0.1:%u\n", sock, (unsigned)port);

    int ok = 1;
    for (int i = 0; i < 3; ++i) {
        int u = uds_ping(sock);
        int v = vsoa_ping(port);
        printf("[2.%d] UDS PING=%s  VSOA /ping=%s\n", i + 1,
               u ? "OK" : "FAIL", v ? "OK" : "FAIL");
        if (!u || !v) ok = 0;
    }
    if (ok) ok = admin_demo(port);
    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(sock); unlink(cfg); unlink(pol); unlink(pf); unlink(tk); rmdir(dir);
    if (ok) printf("=== 双栈同循环可达，示例运行完成! ===\n");
    return ok ? 0 : 1;
}
