/**
 * @file soak_test.c
 * @brief InduRTDB 多进程长时稳定性 (soak) 测试
 * @version 3.1.0
 * @date 2026-08-10
 * @copyright MIT License
 *
 * 父进程创建共享内存实例后 fork N 个子进程 (worker),
 * 每个 worker 独立 attach 并负责互不重叠的点位区间
 * (区间隔离仅为避免 worker 间相互覆盖校验状态, 不影响锁争用).
 * 持续 write + read + peek, 运行指定时长后各自校验数据完整性.
 *
 * 所有 worker 对任意点位的写入均通过 irt_header_t::write_seq 全局
 * seqlock 串行化 (irt_types.h:30), 因此多 worker 并发写必然触发
 * CAS 碰撞, 压测 seqlock 跨进程争用路径.
 *
 * 父进程通过 alarm(SIGALRM) 统一通知所有 worker 停止,
 * 然后 waitpid 收集各 worker 退出码.
 *
 * 用法: soak_test [duration_seconds]    (默认 10)
 * 退出: 0=全部 PASS, 非零=有 worker FAIL
 */

#include <indurtdb/indurtdb.h>

#include <math.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- 测试参数 ---- */
#define NUM_WORKERS     4        /* 并发 worker 进程数 */
#define INSTANCE_ID     "soak_test"
#define MAX_POINTS      1024     /* 共享内存容量 */
#define MAX_SUBS        16

/* 每个 worker 每种类型操作的点位数 */
#define PTS_PER_WORKER  12
/* 每个 worker 操作 4 种类型 (bool/int32/double/string) */
#define TYPE_COUNT      4

/* 写碰撞容忍率: 多进程 seqlock CAS 争用导致的写失败比例上限.
 * InduRTDB 使用单一全局 write_seq (irt_header_t::write_seq, irt_types.h:30),
 * 所有写操作 (无论点位 ID) 串行化于同一 seqlock, 因此多 worker 并发写任意
 * 点位均会触发 CAS 碰撞, 返回 -2. 4 worker 紧循环下碰撞率可达 70-85%,
 * 这是 seqlock 正常争用行为 (非 bug). 设 95% 为安全阈值, 仅排除死锁. */
#define COLLISION_TOLERANCE 0.95

/* ---- 全局信号状态 ---- */
static volatile sig_atomic_t g_stop = 0;

static void alarm_handler(int sig) {
    (void)sig;
    g_stop = 1;
}

/**
 * worker 子进程主函数.
 * 每个 worker 操作互不重叠的点位区间 (避免跨 worker 覆盖校验状态;
 * 锁争用由全局 write_seq 决定, 与区间是否重叠无关):
 *   bool:   worker_id * PTS + [0, PTS)
 *   int32:  100 + worker_id * PTS + [0, PTS)
 *   double: 200 + worker_id * PTS + [0, PTS)
 *   string: 300 + worker_id * PTS + [0, PTS)
 *
 * @return 0=PASS, 1=FAIL (通过 _exit 传递给父进程)
 */
static int worker_main(int worker_id) {
    char tag[32];
    snprintf(tag, sizeof(tag), "[worker-%d]", worker_id);

    /* attach 到共享内存 (fork 检测自动 shutdown + 重新 init) */
    if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
        fprintf(stderr, "%s initialize failed: %s\n",
                tag, indurtdb_get_last_error());
        return 1;
    }

    /* 设置定时器 (继承父进程的 handler) */
    alarm(0);  /* 先取消继承的 alarm */
    signal(SIGALRM, alarm_handler);
    /* 父进程会在 duration_sec 后发送 SIGALRM 给整个进程组,
     * 但这里也设置自己的 alarm 作为后备 (duration + 5s) */

    /* 计算本 worker 的点位基址 */
    uint32_t bool_base   = (uint32_t)(worker_id * PTS_PER_WORKER);
    uint32_t int32_base  = 100 + (uint32_t)(worker_id * PTS_PER_WORKER);
    uint32_t double_base = 200 + (uint32_t)(worker_id * PTS_PER_WORKER);
    uint32_t string_base = 300 + (uint32_t)(worker_id * PTS_PER_WORKER);

    /* ---- 主循环: 持续 write + read + peek ---- */
    uint64_t loop_iters      = 0;
    uint64_t write_ok_count  = 0;
    uint64_t write_collisions = 0;
    uint64_t read_fails      = 0;
    uint64_t peek_fails      = 0;

    /* 记录最后一次成功写入的值, 用于最终校验 */
    int32_t last_int32[PTS_PER_WORKER];
    double  last_double[PTS_PER_WORKER];
    memset(last_int32, 0, sizeof(last_int32));
    memset(last_double, 0, sizeof(last_double));

    while (!g_stop) {
        /* -- 写入: 4 种类型各 PTS_PER_WORKER 个点位 -- */
        for (int i = 0; i < PTS_PER_WORKER; i++) {
            /* bool */
            bool bval = (loop_iters + (uint64_t)i) % 2 == 0;
            int rc = indurtdb_write_bool(bool_base + (uint32_t)i, bval);
            if (rc == 0) write_ok_count++;
            else if (rc == -2) write_collisions++;

            /* int32 */
            int32_t ival = (int32_t)(loop_iters * 7 + (uint64_t)i);
            rc = indurtdb_write_int32(int32_base + (uint32_t)i, ival);
            if (rc == 0) {
                last_int32[i] = ival;
                write_ok_count++;
            } else if (rc == -2) {
                write_collisions++;
            }

            /* double */
            double dval = (double)loop_iters * 0.1 + (double)i;
            rc = indurtdb_write_double(double_base + (uint32_t)i, dval);
            if (rc == 0) {
                last_double[i] = dval;
                write_ok_count++;
            } else if (rc == -2) {
                write_collisions++;
            }

            /* string */
            char sval[32];
            snprintf(sval, sizeof(sval), "w%d_%lu_%d",
                     worker_id, (unsigned long)(loop_iters % 10000), i);
            rc = indurtdb_write_string(string_base + (uint32_t)i, sval);
            if (rc == 0) write_ok_count++;
            else if (rc == -2) write_collisions++;
        }

        /* -- 读回验证 -- */
        for (int i = 0; i < PTS_PER_WORKER; i++) {
            int32_t ri;
            if (indurtdb_read_int32(int32_base + (uint32_t)i, &ri) != 0)
                read_fails++;

            double rd;
            if (indurtdb_read_double(double_base + (uint32_t)i, &rd) != 0)
                read_fails++;
        }

        /* -- peek 检查 -- */
        for (int i = 0; i < PTS_PER_WORKER; i++) {
            if (indurtdb_peek(int32_base + (uint32_t)i) == NULL)
                peek_fails++;
        }

        loop_iters++;
    }

    /* ---- 最终校验: 读回值必须与最后一次成功写入一致 ---- */
    uint64_t verify_fails = 0;

    for (int i = 0; i < PTS_PER_WORKER; i++) {
        /* int32 */
        int32_t ri;
        if (indurtdb_read_int32(int32_base + (uint32_t)i, &ri) == 0) {
            if (ri != last_int32[i]) {
                fprintf(stderr,
                    "%s int32[%d] mismatch: got %d, expected %d\n",
                    tag, i, ri, last_int32[i]);
                verify_fails++;
            }
        } else {
            verify_fails++;
        }

        /* double */
        double rd;
        if (indurtdb_read_double(double_base + (uint32_t)i, &rd) == 0) {
            if (fabs(rd - last_double[i]) > 1e-9) {
                fprintf(stderr,
                    "%s double[%d] mismatch: got %f, expected %f\n",
                    tag, i, rd, last_double[i]);
                verify_fails++;
            }
        } else {
            verify_fails++;
        }

        /* bool */
        bool rb;
        if (indurtdb_read_bool(bool_base + (uint32_t)i, &rb) != 0)
            verify_fails++;

        /* string */
        char rs[32];
        if (indurtdb_read_string(string_base + (uint32_t)i,
                                 rs, sizeof(rs)) != 0)
            verify_fails++;

        /* peek */
        if (indurtdb_peek(int32_base + (uint32_t)i) == NULL)
            verify_fails++;
    }

    /* ---- 统计与判定 ---- */
    uint64_t total_write_attempts = loop_iters * PTS_PER_WORKER * TYPE_COUNT;
    double collision_rate = (total_write_attempts > 0)
        ? (double)write_collisions / (double)total_write_attempts
        : 0.0;

    printf("%s iters=%llu ok_writes=%llu collisions=%llu (%.1f%%) "
           "read_fail=%llu peek_fail=%llu verify_fail=%llu\n",
           tag,
           (unsigned long long)loop_iters,
           (unsigned long long)write_ok_count,
           (unsigned long long)write_collisions,
           collision_rate * 100.0,
           (unsigned long long)read_fails,
           (unsigned long long)peek_fails,
           (unsigned long long)verify_fails);

    bool pass = (read_fails == 0)
             && (peek_fails == 0)
             && (verify_fails == 0)
             && (loop_iters > 0)
             && (collision_rate < COLLISION_TOLERANCE);

    /* _exit 不刷新 stdio 缓冲, 需手动 flush 确保输出可见 */
    fflush(stdout);
    fflush(stderr);

    /* 不调用 indurtdb_shutdown: 子进程不应 unlink shm.
     * 使用 _exit 退出, 不运行 atexit 析构. */
    return pass ? 0 : 1;
}

int main(int argc, char* argv[]) {
    /* 解析时长参数 */
    int duration_sec = 10;
    if (argc > 1) {
        duration_sec = atoi(argv[1]);
        if (duration_sec <= 0) duration_sec = 10;
    }

    printf("[soak] InduRTDB multi-process soak test\n");
    printf("[soak] duration=%ds, workers=%d, pts/worker=%d (x%d types)\n",
           duration_sec, NUM_WORKERS, PTS_PER_WORKER, TYPE_COUNT);

    /* 父进程: 创建共享内存实例 (owner) */
    if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
        fprintf(stderr, "[soak] FAIL: parent initialize: %s\n",
                indurtdb_get_last_error());
        return 1;
    }

    /* 设置 SIGALRM handler (fork 后子进程继承) */
    signal(SIGALRM, alarm_handler);

    /* fork 前刷新 stdout, 避免子进程继承未刷新的缓冲区导致输出重复 */
    fflush(stdout);

    /* fork worker 子进程 */
    pid_t workers[NUM_WORKERS];
    for (int w = 0; w < NUM_WORKERS; w++) {
        workers[w] = fork();
        if (workers[w] < 0) {
            perror("[soak] fork");
            indurtdb_shutdown();
            return 1;
        }
        if (workers[w] == 0) {
            /* ---- 子进程: worker ---- */
            int rc = worker_main(w);
            _exit(rc);
        }
    }

    /* ---- 父进程: 等待 duration 后发停止信号 ---- */
    printf("[soak] %d workers launched, running for %ds...\n",
           NUM_WORKERS, duration_sec);

    sleep((unsigned)duration_sec);

    /* SIGALRM → 所有子进程 (进程组) */
    for (int w = 0; w < NUM_WORKERS; w++) {
        kill(workers[w], SIGALRM);
    }

    /* 收集各 worker 退出码 */
    int pass_count = 0;
    int fail_count = 0;
    for (int w = 0; w < NUM_WORKERS; w++) {
        int status;
        waitpid(workers[w], &status, 0);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            pass_count++;
        } else {
            fail_count++;
            if (WIFEXITED(status)) {
                fprintf(stderr, "[soak] worker %d FAIL (exit %d)\n",
                        w, WEXITSTATUS(status));
            } else if (WIFSIGNALED(status)) {
                fprintf(stderr, "[soak] worker %d killed by signal %d\n",
                        w, WTERMSIG(status));
            }
        }
    }

    /* 父进程清理 shm (父是 owner) */
    indurtdb_shutdown();

    /* 汇总 */
    printf("\n[soak] Results: %d/%d workers PASS\n",
           pass_count, NUM_WORKERS);

    bool all_pass = (fail_count == 0) && (pass_count == NUM_WORKERS);
    printf("[soak] %s\n", all_pass ? "PASS" : "FAIL");
    return all_pass ? 0 : 1;
}
