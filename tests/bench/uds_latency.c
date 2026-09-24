/**
 * @file uds_latency.c
 * @brief Unix Domain Socket 往返延迟基准（v3.3 架构 T0 门槛验证）
 *
 * 目的：验证「UDS 往返延迟落在 5–15 μs」这一架构假设是否成立。
 * 方法：父子进程分别作为 UDS 服务端/客户端，客户端测量 send+recv 往返耗时，
 *       输出 P50 / P99 / P999 / 均值 / 吞吐。
 *
 * 用法（由 scripts/run_uds_bench.sh 调用）：
 *   cc -O2 -DBENCH_WARMUP=20000 -DBENCH_ITERATIONS=200000 uds_latency.c -o uds_latency
 *   ./uds_latency
 */

#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef BENCH_WARMUP
#define BENCH_WARMUP 20000
#endif
#ifndef BENCH_ITERATIONS
#define BENCH_ITERATIONS 200000
#endif

#define BENCH_SOCK_PATH "/tmp/indurtdb_uds_bench.sock"

#ifndef BENCH_MSG_SIZE
#define BENCH_MSG_SIZE 16 /* 默认最小请求；真实写请求约 48B，可用 -D 覆盖 */
#endif

static uint64_t bench_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int bench_cmp_u64(const void* a, const void* b)
{
    uint64_t x = *(const uint64_t*)a;
    uint64_t y = *(const uint64_t*)b;
    return (x > y) - (x < y);
}

static void die(const char* msg)
{
    perror(msg);
    exit(1);
}

/* ---- 服务端：收 16B 回 16B，直到对端关闭 ---- */
static int bench_server(int listen_fd)
{
    int conn = accept(listen_fd, NULL, NULL);
    if (conn < 0) die("accept");

    char buf[BENCH_MSG_SIZE];
    for (;;) {
        ssize_t n = recv(conn, buf, BENCH_MSG_SIZE, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            die("recv");
        }
        if (n == 0) break; /* 客户端已关闭 */
        if (send(conn, buf, (size_t)n, 0) < 0) {
            if (errno == EPIPE) break;
            die("send");
        }
    }
    close(conn);
    return 0;
}

/* ---- 客户端：测量往返延迟 ---- */
static int bench_client(void)
{
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) die("socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BENCH_SOCK_PATH, sizeof(addr.sun_path) - 1);

    /* 等待服务端 listen 就绪 */
    for (;;) {
        if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) == 0) break;
        usleep(1000);
    }

    const long total = BENCH_WARMUP + BENCH_ITERATIONS;
    uint64_t* samples = (uint64_t*)malloc(sizeof(uint64_t) * (size_t)BENCH_ITERATIONS);
    if (!samples) die("malloc");

    char snd[BENCH_MSG_SIZE];
    char rcv[BENCH_MSG_SIZE];
    memset(snd, 0xAB, sizeof(snd));

    uint64_t t_start = bench_now_ns();

    for (long i = 0; i < total; ++i) {
        uint64_t t0 = bench_now_ns();
        if (send(fd, snd, BENCH_MSG_SIZE, 0) < 0) die("send");
        ssize_t got = recv(fd, rcv, BENCH_MSG_SIZE, 0);
        uint64_t t1 = bench_now_ns();
        if (got < 0) die("recv");
        if (i >= BENCH_WARMUP) {
            samples[i - BENCH_WARMUP] = t1 - t0;
        }
    }

    uint64_t t_end = bench_now_ns();
    (void)rcv;

    qsort(samples, (size_t)BENCH_ITERATIONS, sizeof(uint64_t), bench_cmp_u64);

    uint64_t p50 = samples[BENCH_ITERATIONS / 2];
    uint64_t p99 = samples[BENCH_ITERATIONS * 99 / 100];
    uint64_t p999 = samples[BENCH_ITERATIONS * 999 / 1000];
    uint64_t min = samples[0];
    uint64_t max = samples[BENCH_ITERATIONS - 1];

    double sum = 0.0;
    for (long i = 0; i < BENCH_ITERATIONS; ++i) sum += (double)samples[i];
    double mean = sum / (double)BENCH_ITERATIONS;

    double elapsed_s = (double)(t_end - t_start) / 1e9;
    double ops = (double)BENCH_ITERATIONS / elapsed_s;

    printf("============================================\n");
    printf(" InduRTDB UDS Round-Trip Latency Benchmark\n");
    printf("============================================\n");
    printf("WARMUP:      %d\n", BENCH_WARMUP);
    printf("ITERATIONS:  %d\n", BENCH_ITERATIONS);
    printf("MSG_SIZE:    %d B (request + response)\n", BENCH_MSG_SIZE);
    printf("\n");
    printf("min          %8.3f us\n", (double)min / 1000.0);
    printf("mean         %8.3f us\n", mean / 1000.0);
    printf("P50          %8.3f us\n", (double)p50 / 1000.0);
    printf("P99          %8.3f us\n", (double)p99 / 1000.0);
    printf("P99.9        %8.3f us\n", (double)p999 / 1000.0);
    printf("max          %8.3f us\n", (double)max / 1000.0);
    printf("\n");
    printf("throughput   %.2f K round-trip/s (%.2f us avg)\n", ops / 1000.0, (elapsed_s * 1e6) / (double)BENCH_ITERATIONS);
    printf("============================================\n");

    fflush(stdout); /* _exit() 不刷新 stdio 缓冲，必须显式 flush */
    free(samples);
    close(fd);
    return 0;
}

int main(void)
{
    /* 客户端关闭后服务端 send 会收到 SIGPIPE，忽略以免进程被杀 */
    signal(SIGPIPE, SIG_IGN);

    unlink(BENCH_SOCK_PATH);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) die("socket");

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, BENCH_SOCK_PATH, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) die("bind");
    if (listen(listen_fd, 1) < 0) die("listen");

    pid_t pid = fork();
    if (pid < 0) die("fork");

    if (pid == 0) {
        /* 子进程：客户端 */
        int rc = bench_client();
        _exit(rc);
    }

    /* 父进程：服务端 */
    bench_server(listen_fd);

    int status = 0;
    waitpid(pid, &status, 0);

    close(listen_fd);
    unlink(BENCH_SOCK_PATH);

    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "client failed (status=%d)\n", status);
        return 1;
    }
    printf("RESULT: OK\n");
    return 0;
}
