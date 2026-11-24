/**
 * @file rtdbd_agent_main.c
 * @brief h-C 设备侧代理可执行入口（rtdbd-device-agent）。
 *
 * 参数：
 *   --agent-bind <ip>          对外管理网卡 IP（d-A，拒绝 0.0.0.0）
 *   --agent-port <port>        对外端口（0=自动，经 port-file 回读）
 *   --agent-port-file <path>   独立 RTDBD_AGENT_PORT_FILE
 *   --agent-passwd-file <path> 连接级口令文件（d-C，不经命令行）
 *   --upstream-host <ip>       上游 rtdbd 地址（通常 127.0.0.1）
 *   --upstream-port <port>     上游 rtdbd VSOA 端口
 *   --token-ro <token>         plc token（rtdbd 侧预置）
 *   --token-admin <token>      sync_admin token（rtdbd 侧预置）
 *   --max-listeners <n>        并发对外 cli 上限（<=16，u-E）
 */
#include "rtdbd_device_agent.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>

static volatile sig_atomic_t g_stop = 0;
static void on_signal(int sig) { (void)sig; g_stop = 1; }

static const char *arg_value(int argc, char **argv, int *i)
{
    if (*i + 1 >= argc) return NULL;
    return argv[++(*i)];
}

/* 从文件读口令（去掉尾部换行），避免 ps 泄漏（d-C）。 */
static char *read_passwd_file(const char *path)
{
    if (!path) return NULL;
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    char buf[256];
    if (!fgets(buf, sizeof(buf), f)) { fclose(f); return NULL; }
    fclose(f);
    size_t n = strlen(buf);
    while (n > 0u && (buf[n - 1u] == '\n' || buf[n - 1u] == '\r')) buf[--n] = '\0';
    if (n == 0u) return NULL;
    char *out = (char *)malloc(n + 1u);
    if (out) memcpy(out, buf, n + 1u);
    return out;
}

int main(int argc, char **argv)
{
    rtdbd_agent_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.port = 0u;
    cfg.upstream_host = "127.0.0.1";
    cfg.upstream_port = 18999u;
    cfg.max_listeners = 16u;
    const char *passwd_file = NULL;

    for (int i = 1; i < argc; ++i) {
        const char *a = argv[i];
        if (strcmp(a, "--agent-bind") == 0) cfg.bind_ip = arg_value(argc, argv, &i);
        else if (strcmp(a, "--agent-port") == 0) {
            const char *v = arg_value(argc, argv, &i);
            if (v) cfg.port = (uint16_t)atoi(v);
        }
        else if (strcmp(a, "--agent-port-file") == 0) cfg.port_file = arg_value(argc, argv, &i);
        else if (strcmp(a, "--agent-passwd-file") == 0) passwd_file = arg_value(argc, argv, &i);
        else if (strcmp(a, "--upstream-host") == 0) cfg.upstream_host = arg_value(argc, argv, &i);
        else if (strcmp(a, "--upstream-port") == 0) {
            const char *v = arg_value(argc, argv, &i);
            if (v) cfg.upstream_port = (uint16_t)atoi(v);
        }
        else if (strcmp(a, "--token-ro") == 0) cfg.token_ro = arg_value(argc, argv, &i);
        else if (strcmp(a, "--token-admin") == 0) cfg.token_admin = arg_value(argc, argv, &i);
        else if (strcmp(a, "--max-listeners") == 0) {
            const char *v = arg_value(argc, argv, &i);
            if (v) cfg.max_listeners = (uint32_t)atoi(v);
        }
        else {
            fprintf(stderr, "rtdbd-device-agent: unknown arg: %s\n", a);
            return 2;
        }
    }

    char *passwd = read_passwd_file(passwd_file);
    cfg.passwd = passwd;
    if (!cfg.bind_ip || !cfg.passwd || !cfg.token_ro || !cfg.token_admin) {
        fprintf(stderr, "rtdbd-device-agent: --agent-bind/--agent-passwd-file/--token-ro/--token-admin required\n");
        free(passwd);
        return 2;
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    rtdbd_device_agent_t *a = rtdbd_device_agent_create(&cfg);
    if (!a) {
        fprintf(stderr, "rtdbd-device-agent: create failed (check bind/passwd/tokens/upstream)\n");
        free(passwd);
        return 3;
    }
    fprintf(stderr, "rtdbd-device-agent: started on %s:%u\n", cfg.bind_ip, cfg.port);

    while (!g_stop) {
        (void)rtdbd_device_agent_poll_once(a, 50);
    }

    rtdbd_device_agent_destroy(a);
    free(passwd);
    return 0;
}
