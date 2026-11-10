/**
 * @file peers_example.c
 * @brief v3.12 M3.2 ⑤：集群成员管控可运行示例（irtcli SDK）
 *
 * 自包含：默认自己 fork/exec 一个真实 rtdbd（临时 socket / 策略 / 配置，
 * 退出时回收），因此 `make && ./peers_example` 即可直接跑出实测输出，
 * 与 examples/ 既有「自包含示例」惯例一致；也可用 --sock 复用已有服务端。
 *
 * 演示链路（对应 v3.12 M3.2 ②③④）：
 *   irtcli_set_peers → 服务端原子整组替换 + ADD/DEL 入延迟动作队列
 *   irtcli_get_peers → 读到循环尾 drain 后落地的 JOINING 状态
 *   irtcli_set_peers(空) → 清空成员表并确认
 *
 * 编译：CMake target `peers_example`（链 irtcli），或 examples/Makefile。
 */
#include <irtcli/client.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>

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

/* 成员管控往返：返回 0 成功，非 0 失败（便于 ctest 判据）。 */
static int run_demo(const char *sock)
{
    irtcli_t c;
    if (irtcli_init(&c, sock, 0, NULL, NULL) != IRTCLI_OK) {
        fprintf(stderr, "irtcli_init 失败\n");
        return 1;
    }
    irtcli_set_async(&c, false); /* 同步模式，直接等服务端确认 */

    printf("[1] 连接 rtdbd (%s) ...\n", sock);
    if (irtcli_connect(&c) != IRTCLI_OK) {
        fprintf(stderr, "irtcli_connect 失败\n");
        irtcli_close(&c);
        return 1;
    }
    printf("    OK\n");

    irtcli_peer_t peers[3];
    memset(peers, 0, sizeof(peers));
    const char *addrs[] = {"10.20.0.1", "10.20.0.2", "10.20.0.3"};
    const char *nodes[] = {"plc-a", "plc-b", "plc-c"};
    for (uint32_t i = 0; i < 3; ++i) {
        snprintf(peers[i].addr, sizeof(peers[i].addr), "%s", addrs[i]);
        snprintf(peers[i].node_id, sizeof(peers[i].node_id), "%s", nodes[i]);
        peers[i].state = IRTCLI_PEER_ST_UNKNOWN;  /* 终态必须由延迟动作落地 */
    }

    printf("[2] SET_PEERS 3 条（整组替换）...\n");
    if (irtcli_set_peers(&c, peers, 3) != IRTCLI_OK) {
        fprintf(stderr, "irtcli_set_peers 失败\n");
        irtcli_close(&c);
        return 1;
    }
    printf("    OK\n");

    /* 等主循环循环尾 drain 执行（poll 周期 500ms）：ADD(JOINING) -> FULLSYNC(ACTIVE) */
    usleep(1500000);
    printf("[3] GET_PEERS（state=2 ACTIVE：延迟动作 ADD+FULLSYNC 在循环尾闭环 JOINING->ACTIVE）...\n");
    irtcli_peer_t out[IRTCLI_PEER_MAX];
    uint32_t n = 0;
    memset(out, 0, sizeof(out));
    if (irtcli_get_peers(&c, out, IRTCLI_PEER_MAX, &n) != IRTCLI_OK || n != 3u) {
        fprintf(stderr, "irtcli_get_peers 失败或条数不符 (n=%u)\n", n);
        irtcli_close(&c);
        return 1;
    }
    for (uint32_t i = 0; i < n; ++i) {
        printf("    %-12s %-8s state=%u\n", out[i].addr, out[i].node_id, out[i].state);
        if (strcmp(out[i].addr, addrs[i]) != 0) {
            fprintf(stderr, "成员地址不一致\n");
            irtcli_close(&c);
            return 1;
        }
        /* 服务端把 PEER_ADD 入延迟动作队列、在主循环循环尾执行：先落 JOINING
         * 再由 TRIGGER_FULLSYNC 推进 ACTIVE（成员接纳状态机）。请求携带 UNKNOWN(0)，
         * 若延迟队列是死代码，整组替换会原样落库 → 读回 UNKNOWN(0)；故本示例
         * 同时是「延迟队列已接线 + 状态机闭环」的可证伪判据。 */
        if (out[i].state != IRTCLI_PEER_ST_ACTIVE) {
            fprintf(stderr, "成员 %s 状态=%u，期望 ACTIVE(2)：延迟动作 ADD+FULLSYNC 未在循环尾闭环\n",
                    out[i].addr, out[i].state);
            irtcli_close(&c);
            return 1;
        }
    }

    /* v3.12 M3.3：本节点地址（子进程 --sync-self-addr 10.20.0.99）必须被拒绝，
     * 且拒绝后旧表保持不变 —— 服务端安全边界的端到端演示。 */
    printf("[3.1] 尝试 SET_PEERS 含本节点地址 10.20.0.99（应被拒）...\n");
    irtcli_peer_t bad[2];
    memset(bad, 0, sizeof(bad));
    snprintf(bad[0].addr, sizeof(bad[0].addr), "10.3.3.3");
    snprintf(bad[0].node_id, sizeof(bad[0].node_id), "other");
    snprintf(bad[1].addr, sizeof(bad[1].addr), "10.20.0.99");
    snprintf(bad[1].node_id, sizeof(bad[1].node_id), "self");
    bad[0].state = bad[1].state = IRTCLI_PEER_ST_ACTIVE;
    if (irtcli_set_peers(&c, bad, 2) == IRTCLI_OK) {
        fprintf(stderr, "含自地址的 SET_PEERS 未被拒绝\n");
        irtcli_close(&c);
        return 1;
    }
    uint32_t kept = 999u;
    if (irtcli_get_peers(&c, out, IRTCLI_PEER_MAX, &kept) != IRTCLI_OK || kept != 3u) {
        fprintf(stderr, "拒绝自地址后旧表未保持 3 条 (kept=%u)\n", kept);
        irtcli_close(&c);
        return 1;
    }
    printf("    OK（拒绝且旧表 3 条不变）\n");

    printf("[4] SET_PEERS 空表（清空）...\n");
    if (irtcli_set_peers(&c, NULL, 0) != IRTCLI_OK) {
        fprintf(stderr, "清空成员表失败\n");
        irtcli_close(&c);
        return 1;
    }
    printf("[5] GET_PEERS 确认 0 条...\n");
    n = 999u;
    if (irtcli_get_peers(&c, out, IRTCLI_PEER_MAX, &n) != IRTCLI_OK || n != 0u) {
        fprintf(stderr, "清空后仍有 %u 条成员\n", n);
        irtcli_close(&c);
        return 1;
    }
    printf("    OK (0 条)\n");

    irtcli_close(&c);
    return 0;
}

static void usage(const char *prog)
{
    printf("用法: %s [--sock PATH] [--help]\n"
           "  默认自包含启动 rtdbd；--sock 复用已有服务端（不启动/回收）。\n", prog);
}

int main(int argc, char **argv)
{
    const char *sock_arg = NULL;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--sock") == 0 && i + 1 < argc) {
            sock_arg = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0) {
            usage(argv[0]);
            return 0;
        } else {
            usage(argv[0]);
            return 1;
        }
    }

    printf("=== InduRTDB 集群成员管控示例 (irtcli SDK, v3.12 M3.2) ===\n");
    if (sock_arg) return run_demo(sock_arg);

    /* ---- 自包含：临时目录 + 策略/配置 + fork rtdbd ---- */
    char dir[] = "/tmp/indurtdb_peers_example_XXXXXX";
    if (!mkdtemp(dir)) {
        fprintf(stderr, "mkdtemp 失败\n");
        return 1;
    }
    char sock[256], cfg[256], pol[256], inst[128], policy_body[128];
    snprintf(sock, sizeof(sock), "%s/rtdbd.sock", dir);
    snprintf(cfg, sizeof(cfg), "%s/cfg.yaml", dir);
    snprintf(pol, sizeof(pol), "%s/policy.txt", dir);
    snprintf(inst, sizeof(inst), "peers_example_%d", (int)getpid());
    snprintf(policy_body, sizeof(policy_body), "%u:*:0:999\n", (unsigned)getuid());

    if (write_text(cfg, "points: []\n") != 0 || write_text(pol, policy_body) != 0) {
        fprintf(stderr, "写临时配置失败\n");
        return 1;
    }

    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "fork 失败\n");
        return 1;
    }
    if (pid == 0) {
        execl(RTDBD_BIN, "rtdbd", "--socket", sock, "--instance", inst,
              "--config", cfg, "--policy", pol,
              "--max-points", "64", "--max-subs", "4",
              "--sync-self-addr", "10.20.0.99", (char *)NULL);
        _exit(127);
    }

    int ready = 0;
    for (int i = 0; i < 200; ++i) {
        struct stat st;
        if (stat(sock, &st) == 0) { ready = 1; break; }
        usleep(10000);
    }
    if (!ready) {
        fprintf(stderr, "rtdbd 未就绪（%s）\n", RTDBD_BIN);
        kill(pid, SIGTERM);
        waitpid(pid, NULL, 0);
        return 1;
    }

    int rc = run_demo(sock);

    kill(pid, SIGTERM);
    waitpid(pid, NULL, 0);
    unlink(sock); unlink(cfg); unlink(pol); rmdir(dir);

    if (rc == 0) printf("=== 示例运行完成! ===\n");
    return rc;
}
