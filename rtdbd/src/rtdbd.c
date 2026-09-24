/**
 * @file rtdbd.c
 * @brief rtdbd —— 写权威服务进程（控制面 OT）
 *
 * T1 骨架：UDS 监听 + 协议解析 + 串行处理。
 * 当前为 TDD 的「红」阶段骨架：WRITE 返回 NOT_IMPLEMENTED，
 * 待 T1 实现提交后替换为真实写入路径（串行写 + 鉴权 + 审计）。
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>

#include "audit.h"
#include "policy.h"

#define RTDBD_MAX_CLIENTS 32
#define RTDBD_SHUTDOWN_DELAY_SEC 1

static volatile sig_atomic_t g_stop = 0;

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static int send_all(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void* buf, size_t len)
{
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* 对端关闭 */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* 返回 0 表示连接可继续保持；非 0 表示需关闭连接 */
static int handle_request(int fd, irt_policy_t* policy, irt_audit_t* audit)
{
    rtdbd_req_hdr_t req;
    if (recv_all(fd, &req, sizeof(req)) != 0) return 1;

    if (req.magic != RTDBD_MAGIC || req.version != RTDBD_PROTO_VERSION) {
        /* 版本不匹配：不静默，直接关闭连接 */
        return 1;
    }

    rtdbd_resp_hdr_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic   = RTDBD_MAGIC;
    resp.version = RTDBD_PROTO_VERSION;

    if (req.opcode == RTDBD_OP_PING) {
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = 0;
        (void)policy;
        (void)audit;
        return send_all(fd, &resp, sizeof(resp));
    }

    if (req.opcode == RTDBD_OP_AUDIT_DUMP) {
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = 0; /* 红阶段：审计尚未接入 */
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        return 0;
    }

    if (req.opcode == RTDBD_OP_WRITE) {
        rtdbd_write_req_t w;
        if (req.payload_len != sizeof(w) || recv_all(fd, &w, sizeof(w)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }
        /* 红阶段：尚未实现写入 */
        resp.status      = RTDBD_ST_NOT_IMPLEMENTED;
        resp.payload_len = 0;
        return send_all(fd, &resp, sizeof(resp));
    }

    resp.status      = RTDBD_ST_BAD_REQUEST;
    resp.payload_len = 0;
    (void)send_all(fd, &resp, sizeof(resp));
    return 1;
}

int main(int argc, char** argv)
{
    const char* sock_path  = "/run/indurtdb/default.sock";
    const char* instance   = "default";
    const char* policy_path = NULL;
    uint32_t    max_points = 10000;
    uint32_t    max_subs   = 32;

    static struct option long_opts[] = {
        {"socket",     required_argument, 0, 's'},
        {"instance",   required_argument, 0, 'i'},
        {"policy",     required_argument, 0, 'p'},
        {"max-points", required_argument, 0, 'm'},
        {"max-subs",   required_argument, 0, 'b'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:i:p:m:b:", long_opts, NULL)) != -1) {
        switch (c) {
        case 's': sock_path = optarg; break;
        case 'i': instance = optarg; break;
        case 'p': policy_path = optarg; break;
        case 'm': max_points = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'b': max_subs = (uint32_t)strtoul(optarg, NULL, 10); break;
        default: break;
        }
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    irt_policy_t policy;
    irt_policy_init(&policy);
    if (policy_path && irt_policy_load(&policy, policy_path) != 0) {
        fprintf(stderr, "rtdbd: failed to load policy: %s\n", policy_path);
        return 1;
    }

    irt_audit_t audit;
    irt_audit_init(&audit);

    if (indurtdb_initialize(instance, max_points, max_subs) != 0) {
        fprintf(stderr, "rtdbd: indurtdb_initialize failed: %s\n", indurtdb_get_last_error());
        return 1;
    }

    unlink(sock_path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listen_fd, 8) < 0) {
        perror("bind/listen");
        close(listen_fd);
        return 1;
    }

    printf("rtdbd listening on %s (instance=%s)\n", sock_path, instance);
    fflush(stdout);

    int clients[RTDBD_MAX_CLIENTS];
    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) clients[i] = -1;

    while (!g_stop) {
        struct pollfd fds[RTDBD_MAX_CLIENTS + 1];
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        int nfds = 1;

        for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
            if (clients[i] >= 0) {
                fds[nfds].fd = clients[i];
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (fds[0].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int placed = 0;
                for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
                    if (clients[i] < 0) {
                        clients[i] = cfd;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(cfd);
            }
        }

        for (int i = 1; i < nfds; ++i) {
            if (!(fds[i].revents & POLLIN)) continue;
            if (handle_request(fds[i].fd, &policy, &audit) != 0) {
                close(fds[i].fd);
                for (int k = 0; k < RTDBD_MAX_CLIENTS; ++k) {
                    if (clients[k] == fds[i].fd) {
                        clients[k] = -1;
                        break;
                    }
                }
            }
        }
    }

    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
        if (clients[i] >= 0) close(clients[i]);
    }
    close(listen_fd);
    unlink(sock_path);

    indurtdb_shutdown();
    (void)now_ns();
    (void)RTDBD_SHUTDOWN_DELAY_SEC;
    return 0;
}
