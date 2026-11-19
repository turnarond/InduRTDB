/**
 * @file sylixos_smoke.c
 * @brief SylixOS 单台功能测试 harness（G0/G1，非 ctest 交付证据）
 * @version 3.12.0
 * @date 2026-11-19
 *
 * 覆盖可单台验证的五类语义（SylixOS 平台适配版）：
 *   1. OSAL 时间单调性 / 墙钟非零非递减（不假设 Unix epoch——PLC RTC 可为 2000）
 *   2. 点位写入-读回一致性（v2 句柄 API）—— v3.12 atomic64 修复的核心验证点
 *   3. fork 子进程 attach（SylixOS 无 fork → 标 N/A，非产品缺陷）
 *   4. reopen 一致性（崩溃恢复后的读回）
 *   5. 多线程并发写（3 线程各 40 次）写锁串行 + write_count 精确累加
 *      —— v3.12 atomic64 修复（非对齐 8 字节原子）的可证伪用例
 *
 * 用法：sylixos_smoke <instance_id>
 * 每类以 "[CASE n] PASS/FAIL/N/A" 打印；无 FAIL 打印 "SMOKE_RESULT: PASS" 并 exit 0
 * （N/A 不计失败），有 FAIL 打印 "SMOKE_RESULT: FAIL" 并 exit 1。断言+退出码才是证据。
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
#include <pthread.h>

static int g_fail = 0;

static void case_result(int idx, int ok, const char* detail) {
    printf("[CASE %d] %s%s%s\n", idx, ok ? "PASS" : "FAIL",
           detail ? " : " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* 平台不支持的用例标 N/A，不计入失败（如实记录，不伪造 PASS） */
static int g_na = 0;
static void case_na(int idx, const char* reason) {
    printf("[CASE %d] N/A : %s\n", idx, reason);
    g_na++;
}

/* 1. OSAL 时间：单调钟非递减且 sleep 后前进；墙钟非零且非递减。
 *    v3.12 修正：不假设墙钟是 Unix epoch——19 号方案明确「PLC 设备时钟不可信，
 *    物理时间戳不参与排序」，实测设备 RTC=2000-01-01（掉电后无电池/NTP）。
 *    墙钟仅用于对外展示的入库时刻，业务排序用单调钟。 */
static void test_time(void) {
    uint64_t t0 = irt_time_now_ns();
    uint64_t w0 = irt_time_realtime_ns();
    irt_time_sleep_ns(1000000ULL); /* 1 ms */
    uint64_t t1 = irt_time_now_ns();
    uint64_t w1 = irt_time_realtime_ns();

    int mono_ok = (t1 >= t0);
    int wall_ok = (w0 != 0 && w1 >= w0); /* 非零 + 非递减，不断言 epoch 起点 */
    int adv_ok = (t1 > t0);
    char detail[160];
    snprintf(detail, sizeof(detail),
             "mono t0=%llu t1=%llu wall0=%llu wall1=%llu(rtc-as-is)",
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
        /* SylixOS 3.6.5 无 fork（进程由 servermgr 管理），非产品缺陷，标 N/A */
        case_na(3, "fork unavailable on SylixOS 3.6.5 (process model = servermgr)");
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

/* 5. 多线程并发写（判据 4）：SylixOS 无 fork，改用 3 线程共享句柄各写 40 次，
 *    写锁冲突时退避重试直到成功；验证无崩溃且 write_count 增量 == 3*40。
 *    并发写触及修复点（锁内普通自增）：若仍有非对齐 8 字节原子必崩（可证伪）；
 *    若锁内自增在并发下丢失更新，增量 < 期望（可证伪）。
 *    注：多线程与多进程共用同一 seqlock+stats 语义（同一共享 header），
 *    此用例验证修复核心（并发下计数正确），跨平台进程模型的差异另记。 */
#define SMOKE_NTHREAD 3
#define SMOKE_NWRITES 40

typedef struct {
    indurtdb_t* db;
    int idx;
    int done;
} smoke_writer_t;

static void* smoke_writer_thread(void* arg) {
    smoke_writer_t* w = (smoke_writer_t*)arg;
    for (int t = 0; t < SMOKE_NWRITES * 20 && w->done < SMOKE_NWRITES; ++t) {
        int32_t v = (int32_t)(w->idx * 1000 + w->done);
        /* 各线程写独立点位 id=10+idx；只关心计数，不读回值 */
        if (indurtdb_h_write_int32(w->db, (uint32_t)(10 + w->idx), v) == 0) {
            ++w->done;
        } else {
            irt_time_sleep_ns(1000000ULL); /* 1ms 退避，写锁冲突重试 */
        }
    }
    return NULL;
}

static void test_concurrent_writes(const char* instance) {
    indurtdb_t* db = NULL;
    indurtdb_cfg_t cfg = { .max_points = 64, .max_subscribers = 8 };
    if (indurtdb_h_open(&db, instance, &cfg) != 0) {
        case_result(5, 0, indurtdb_get_last_error());
        return;
    }
    uint64_t base = indurtdb_h_get_write_count(db);

    pthread_t tids[SMOKE_NTHREAD];
    smoke_writer_t writers[SMOKE_NTHREAD];
    int created = 0;
    for (int i = 0; i < SMOKE_NTHREAD; ++i) {
        writers[i].db = db;
        writers[i].idx = i;
        writers[i].done = 0;
        if (pthread_create(&tids[i], NULL, smoke_writer_thread, &writers[i]) == 0) {
            ++created;
        } else {
            break;
        }
    }

    int threads_ok = (created == SMOKE_NTHREAD);
    for (int i = 0; i < created; ++i) {
        pthread_join(tids[i], NULL);
        if (writers[i].done != SMOKE_NWRITES) threads_ok = 0;
    }

    uint64_t after = indurtdb_h_get_write_count(db);
    uint64_t delta = after - base;
    uint64_t expect = (uint64_t)(SMOKE_NTHREAD * SMOKE_NWRITES);
    int ok = threads_ok && (delta == expect);

    char detail[160];
    snprintf(detail, sizeof(detail), "delta=%llu expect=%llu threads_ok=%d",
             (unsigned long long)delta, (unsigned long long)expect, threads_ok);
    case_result(5, ok, detail);
    indurtdb_h_close(db);
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
    test_concurrent_writes(instance);

    if (g_fail == 0) {
        printf("SMOKE_RESULT: PASS (%d passed, %d N/A)\n", 5 - g_na, g_na);
        return 0;
    }
    printf("SMOKE_RESULT: FAIL (%d failed, %d N/A)\n", g_fail, g_na);
    return 1;
}
