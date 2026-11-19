/**
 * @file sylixos_smoke.c
 * @brief SylixOS 单台功能测试 harness（G0/G1，非 ctest 交付证据）
 * @version 3.12.0
 * @date 2026-11-19
 *
 * 覆盖可单台验证的四类语义：
 *   1. OSAL 时间单调性 / 墙钟可用性
 *   2. 点位写入-读回一致性（v2 句柄 API）
 *   3. fork 后子进程 attach 同一共享内存段（owner vs attacher）
 *   4. 遗留奇数 write_seq 的崩溃恢复
 *
 * 用法：sylixos_smoke <instance_id>
 * 每类以 "[CASE n] PASS/FAIL" 打印；全部通过打印 "SMOKE_RESULT: PASS" 并 exit 0，
 * 任一失败打印 "SMOKE_RESULT: FAIL" 并 exit 1。字符串回显不作为证据，断言+退出码才是。
 *
 * 注意：SylixOS shell 不支持 '<'/'>' 重定向与 '&&'，输出直接到终端，由 telnet 回显取证。
 */

#include <indurtdb/indurtdb.h>
#include "osal/irt_osal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

static int g_fail = 0;

static void case_result(int idx, int ok, const char* detail) {
    printf("[CASE %d] %s%s%s\n", idx, ok ? "PASS" : "FAIL",
           detail ? " : " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* 1. OSAL 时间：单调钟非递减、墙钟为 Unix epoch 量级、sleep 后单调钟前进 */
static void test_time(void) {
    uint64_t t0 = irt_time_now_ns();
    uint64_t w0 = irt_time_realtime_ns();
    irt_time_sleep_ns(1000000ULL); /* 1 ms */
    uint64_t t1 = irt_time_now_ns();
    uint64_t w1 = irt_time_realtime_ns();

    int mono_ok = (t1 >= t0);
    int wall_ok = (w0 > 1600000000000000000ULL && w1 >= w0); /* 2020 年后且非递减：是 epoch 而非单调起点 */
    int adv_ok = (t1 > t0);
    char detail[160];
    snprintf(detail, sizeof(detail),
             "mono t0=%llu t1=%llu wall0=%llu wall1=%llu",
             (unsigned long long)t0, (unsigned long long)t1,
             (unsigned long long)w0, (unsigned long long)w1);
    case_result(1, mono_ok && wall_ok && adv_ok, detail);
}

/* 2. 点位写入-读回一致性 */
static void test_rw(const char* instance) {
    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 64, .max_subscribers = 8 };
    if (indurtdb_h_open(&db, instance, &cfg) != 0) {
        case_result(2, 0, indurtdb_get_last_error());
        return;
    }
    indurtdb_h_write_double(db, 1, 23.5);
    indurtdb_h_write_int32(db, 2, -7);
    indurtdb_h_write_bool(db, 3, true);

    indurtdb_point_t p;
    int ok = 1;
    if (indurtdb_h_read_point(db, 1, &p) != 0 || p.value.d < 23.49 || p.value.d > 23.51) ok = 0;
    if (indurtdb_h_read_point(db, 2, &p) != 0 || p.value.i != -7) ok = 0;
    if (indurtdb_h_read_point(db, 3, &p) != 0 || !p.value.b) ok = 0;

    char detail[128];
    snprintf(detail, sizeof(detail), "writes=%llu",
             (unsigned long long)indurtdb_h_get_write_count(db));
    case_result(2, ok, detail);
    indurtdb_h_close(db);
}

/* 3. fork 子进程 attach 同一段：子进程应能读回父进程写入的值 */
static void test_fork_attach(const char* instance) {
    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 64, .max_subscribers = 8 };
    if (indurtdb_h_open(&db, instance, &cfg) != 0) {
        case_result(3, 0, indurtdb_get_last_error());
        return;
    }
    indurtdb_h_write_int32(db, 5, 424242);

    pid_t pid = fork();
    if (pid < 0) {
        case_result(3, 0, "fork failed");
        indurtdb_h_close(db);
        return;
    }
    if (pid == 0) {
        /* 子进程：重新 attach 同一实例 */
        indurtdb_t* cdb = NULL;
        int rc = indurtdb_h_open(&cdb, instance, &cfg);
        if (rc != 0) { _exit(10); }
        indurtdb_point_t p;
        int rd = indurtdb_h_read_point(cdb, 5, &p);
        int val_ok = (rd == 0 && p.value.i == 424242);
        indurtdb_h_close(cdb);
        _exit(val_ok ? 0 : 11);
    }

    /* 父进程等待子进程 */
    int status = 0;
    waitpid(pid, &status, 0);
    int ok = WIFEXITED(status) && (WEXITSTATUS(status) == 0);
    char detail[96];
    snprintf(detail, sizeof(detail), "child_exit=%d", WIFEXITED(status) ? WEXITSTATUS(status) : -1);
    case_result(3, ok, detail);
    indurtdb_h_close(db);
}

/* 4. 崩溃恢复：在共享内存 header 上构造奇数 write_seq，重新 attach 应被推进为偶数。
 *    通过公共 API 无法直接写 header，故本用例验证「重复 open/close 后读写仍可用」
 *    作为最小可证的恢复后一致性；奇数 seq 的字面量推进由 test_c_layout_seqlock 覆盖（Linux 侧）。 */
static void test_reopen_consistency(const char* instance) {
    indurtdb_t* a = NULL;
    indurtdb_cfg_t cfg = { .max_points = 64, .max_subscribers = 8 };
    if (indurtdb_h_open(&a, instance, &cfg) != 0) {
        case_result(4, 0, indurtdb_get_last_error());
        return;
    }
    indurtdb_h_write_double(a, 9, 88.25);
    indurtdb_h_close(a);

    indurtdb_t* b = NULL;
    int ok = 1;
    if (indurtdb_h_open(&b, instance, &cfg) != 0) {
        ok = 0;
    } else {
        indurtdb_point_t p;
        if (indurtdb_h_read_point(b, 9, &p) != 0 || p.value.d < 88.24 || p.value.d > 88.26) ok = 0;
        indurtdb_h_close(b);
    }
    case_result(4, ok, ok ? "reopen readback consistent" : "reopen failed");
}

int main(int argc, char** argv) {
    if (argc < 2) {
        printf("usage: sylixos_smoke <instance_id>\n");
        printf("SMOKE_RESULT: FAIL\n");
        return 1;
    }
    const char* instance = argv[1];

    printf("=== InduRTDB SylixOS smoke (instance=%s) ===\n", instance);
    test_time();
    test_rw(instance);
    test_fork_attach(instance);
    test_reopen_consistency(instance);

    if (g_fail == 0) {
        printf("SMOKE_RESULT: PASS (4/4)\n");
        return 0;
    }
    printf("SMOKE_RESULT: FAIL (%d case(s) failed)\n", g_fail);
    return 1;
}
