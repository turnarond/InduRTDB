/**
 * @file fault_injection_test.c
 * @brief InduRTDB 崩溃恢复故障注入测试
 * @version 3.1.0
 * @date 2026-08-10
 * @copyright MIT License
 *
 * 架构: "supervisor" 模式 — 主进程 (supervisor) 不初始化 RTDB,
 * 仅 fork 一个 owner 子进程, 然后 kill 之, 再由 supervisor 自己
 * initialize 触发崩溃恢复. 这避免了 fork 后全局单例状态污染.
 *
 * 场景 1 — kill-owner 恢复:
 *   fork owner → owner initialize + 写已知数据 → SIGKILL owner →
 *   supervisor initialize (非 owner attach) →
 *   检测到 owner_pid 死亡 → claim_ownership → 验证数据 + 读写
 *
 * 场景 2 — 奇数 write_seq 恢复 (模拟写锁内崩溃):
 *   fork owner → owner 写数据 → supervisor 外部操纵 write_seq 为奇数
 *   (模拟 owner 在 seqlock write_begin/write_end 间被 SIGKILL) →
 *   SIGKILL owner → supervisor initialize → 检测奇数 write_seq
 *   并推进至偶数 → 验证数据完整 + 可读写
 *
 * owner 子进程一律 _exit() (或被 SIGKILL), 保证不运行 atexit
 * 析构/shm_unlink.
 *
 * 退出: 0=PASS, 非零=FAIL
 */

#include <indurtdb/indurtdb.h>

#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* ---- 测试参数 ---- */
#define NUM_POINTS   10
#define INSTANCE_ID  "fi_test"
/* 共享内存段 POSIX 名称 (与 IRT_SHM_PREFIX 一致) */
#define SHM_NAME     "/indurtdb_" INSTANCE_ID
/* 须与 indurtdb_initialize 参数一致 */
#define MAX_POINTS   256
#define MAX_SUBS     16

/* write_seq 在 irt_header_t 中的字节偏移:
 * magic(4) + version(4) + max_points(4) + max_subscribers(4) = 16 */
#define WRITE_SEQ_OFFSET 16

/* 点位数据大小 (与 indurtdb_point_t 一致) */
#define POINT_SIZE   128
/* 头部大小 */
#define HEADER_SIZE  64

/* 段总大小 */
#define SHM_TOTAL_SIZE \
    (HEADER_SIZE + (size_t)MAX_POINTS * POINT_SIZE + (size_t)MAX_SUBS * 16)

/* ---- 辅助函数 ---- */

/**
 * 外部操纵共享内存 write_seq (绕过 API).
 * 将偶数 write_seq 推进 1 变为奇数, 模拟 owner 在
 * irt_seqlock_write_begin (CAS 成功, seq 变奇) 和
 * irt_seqlock_write_end (seq 变下一个偶) 之间被 SIGKILL.
 * 返回 0=成功.
 */
static int corrupt_write_seq_odd(void) {
    int fd = shm_open(SHM_NAME, O_RDWR, 0666);
    if (fd < 0) {
        perror("[fi] shm_open for corruption");
        return -1;
    }

    void* base = mmap(NULL, SHM_TOTAL_SIZE,
                      PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) {
        perror("[fi] mmap for corruption");
        return -1;
    }

    uint64_t* seq = (uint64_t*)((char*)base + WRITE_SEQ_OFFSET);
    uint64_t cur = __atomic_load_n(seq, __ATOMIC_ACQUIRE);

    if (!(cur & 1ULL)) {
        __atomic_store_n(seq, cur + 1, __ATOMIC_RELEASE);
        printf("[fi]   write_seq corrupted: %llu -> %llu (odd)\n",
               (unsigned long long)cur, (unsigned long long)(cur + 1));
    } else {
        printf("[fi]   write_seq already odd: %llu\n",
               (unsigned long long)cur);
    }

    munmap(base, SHM_TOTAL_SIZE);
    return 0;
}

/**
 * 打开共享内存并读取 write_seq.
 * 返回: >=0 时的 write_seq 值, -1 表示错误.
 */
static int64_t peek_write_seq(void) {
    int fd = shm_open(SHM_NAME, O_RDONLY, 0666);
    if (fd < 0) return -1;

    void* base = mmap(NULL, SHM_TOTAL_SIZE, PROT_READ, MAP_SHARED, fd, 0);
    close(fd);
    if (base == MAP_FAILED) return -1;

    uint64_t* seq = (uint64_t*)((char*)base + WRITE_SEQ_OFFSET);
    uint64_t val = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    munmap(base, SHM_TOTAL_SIZE);
    return (int64_t)val;
}

/**
 * fork owner 子进程, owner 调用 indurtdb_initialize 并写入已知数据.
 * 成功返回 owner PID, 失败返回 -1.
 */
static pid_t spawn_owner(void) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("[fi] fork");
        return -1;
    }
    if (pid == 0) {
        /* ---- Owner child ---- */
        if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
            fprintf(stderr, "[fi]   owner initialize failed: %s\n",
                    indurtdb_get_last_error());
            _exit(1);
        }

        /* 写入 int32 + double */
        for (uint32_t i = 0; i < NUM_POINTS; i++) {
            indurtdb_write_int32(i, (int32_t)(i * 100 + 42));
            indurtdb_write_double(NUM_POINTS + i, (double)i * 1.5 + 0.5);
        }

        /* 不调用 indurtdb_shutdown: 模拟崩溃, 保留 shm.
         * _exit 不运行 atexit, 不执行 shm_unlink. */
        _exit(0);
    }
    return pid;
}

/**
 * 等待 owner 子进程退出或被 kill.
 * 返回 0=正常退出/被信号杀死, -1=waitpid 错误.
 */
static int wait_owner(pid_t owner) {
    int status;
    if (waitpid(owner, &status, 0) < 0) {
        perror("[fi] waitpid");
        return -1;
    }
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status) == 0 ? 0 : -1;
    }
    if (WIFSIGNALED(status)) {
        return 0;  /* 被信号杀死是预期行为 */
    }
    return -1;
}

/* ============================================================
 * 场景 1: kill-owner 崩溃恢复
 *
 * 覆盖代码路径 (irt_shm.c):
 *   kill(stored_pid, 0) → ESRCH → irt_shm_os_claim_ownership()
 *   + hdr->owner_pid 更新
 * ============================================================ */
static int test_kill_owner(void) {
    printf("\n[fi] === Scenario 1: kill-owner recovery ===\n");

    /* Step 1: fork owner 子进程 */
    pid_t owner = spawn_owner();
    if (owner < 0) return 1;

    /* Step 2: 等待 owner 完成写入并退出 */
    if (wait_owner(owner) != 0) {
        fprintf(stderr, "[fi]   owner child failed\n");
        return 1;
    }
    printf("[fi]   owner (pid=%d) wrote data and exited\n", (int)owner);

    /* owner _exit(0) 后 PID 已死, 但 shm 仍存在 (owner 没 unlink).
     * 注意: owner 正常退出也不 unlink, 因为 _exit 跳过 atexit,
     * 且 indurtdb_shutdown 未被调用. 这精确模拟了 SIGKILL 崩溃. */

    /* Step 3: supervisor 初始化 → 触发崩溃恢复 */
    if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
        fprintf(stderr, "[fi]   supervisor initialize failed: %s\n",
                indurtdb_get_last_error());
        return 1;
    }
    printf("[fi]   supervisor recovered ownership (pid=%d)\n", (int)getpid());

    /* Step 4: 验证 owner 崩溃前写入的数据完整 */
    int data_ok = 1;
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i * 100 + 42)) {
            fprintf(stderr, "[fi]   int32[%u] mismatch: got %d, want %d\n",
                    i, ri, (int)(i * 100 + 42));
            data_ok = 0;
        }
        double rd;
        if (indurtdb_read_double(NUM_POINTS + i, &rd) != 0) {
            fprintf(stderr, "[fi]   double[%u] read failed\n", i);
            data_ok = 0;
        }
    }
    if (!data_ok) {
        fprintf(stderr, "[fi]   pre-crash data verification FAILED\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   pre-crash data verified OK\n");

    /* Step 5: 验证恢复后可正常写入新数据 */
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        indurtdb_write_int32(i, (int32_t)(i * 200 + 99));
    }
    int write_ok = 1;
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i * 200 + 99)) {
            write_ok = 0;
        }
    }
    if (!write_ok) {
        fprintf(stderr, "[fi]   post-recovery write/read FAILED\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   post-recovery write/read OK\n");

    indurtdb_shutdown();  /* supervisor 是 owner, 清理 shm */
    return 0;
}

/* ============================================================
 * 场景 2: 奇数 write_seq 恢复 (模拟写锁内崩溃)
 *
 * 覆盖代码路径 (irt_shm.c):
 *   seq = __atomic_load_n(&hdr->write_seq, ACQUIRE);
 *   if (seq & 1ULL) {
 *       __atomic_store_n(&hdr->write_seq, seq + 1, RELEASE);
 *   }
 * ============================================================ */
static int test_odd_write_seq(void) {
    printf("\n[fi] === Scenario 2: odd write_seq recovery ===\n");

    /* Step 1: fork owner 子进程写入数据 */
    pid_t owner = spawn_owner();
    if (owner < 0) return 1;

    if (wait_owner(owner) != 0) {
        fprintf(stderr, "[fi]   owner child failed\n");
        return 1;
    }
    printf("[fi]   owner (pid=%d) wrote data\n", (int)owner);

    /* Step 2: 确认 write_seq 当前为偶数 (owner 正常完成所有写入) */
    int64_t seq_before = peek_write_seq();
    printf("[fi]   write_seq after owner: %lld (should be even)\n",
           (long long)seq_before);
    if (seq_before < 0 || (seq_before & 1LL)) {
        fprintf(stderr, "[fi]   unexpected odd write_seq before corruption\n");
        return 1;
    }

    /* Step 3: 外部操纵 write_seq → 奇数.
     * 模拟 owner 在 irt_seqlock_write_begin (CAS 将 seq 从偶变奇)
     * 和 irt_seqlock_write_end (将 seq 推进至下一个偶) 之间被 SIGKILL.
     * 点位数据本身是完整的 (上一次写入已完成), 只有 seqlock 状态不一致. */
    if (corrupt_write_seq_odd() != 0) {
        fprintf(stderr, "[fi]   failed to corrupt write_seq\n");
        return 1;
    }

    /* 确认 write_seq 现在为奇数 */
    int64_t seq_corrupt = peek_write_seq();
    printf("[fi]   write_seq after corruption: %lld (odd)\n",
           (long long)seq_corrupt);

    /* Step 4: supervisor 初始化 → 触发奇数 write_seq 恢复 */
    if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
        fprintf(stderr, "[fi]   supervisor initialize failed: %s\n",
                indurtdb_get_last_error());
        return 1;
    }
    printf("[fi]   supervisor recovered (pid=%d)\n", (int)getpid());

    /* Step 5: 验证 write_seq 已恢复为偶数 */
    int64_t seq_after = peek_write_seq();
    printf("[fi]   write_seq after recovery: %lld (should be even)\n",
           (long long)seq_after);
    if (seq_after & 1LL) {
        fprintf(stderr,
            "[fi]   write_seq still odd after recovery: %lld\n",
            (long long)seq_after);
        indurtdb_shutdown();
        return 1;
    }

    /* Step 6: 验证数据完整 (写入时数据已持久化, 只有 seqlock 被遗留) */
    int data_ok = 1;
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i * 100 + 42)) {
            fprintf(stderr, "[fi]   int32[%u] mismatch: got %d, want %d\n",
                    i, ri, (int)(i * 100 + 42));
            data_ok = 0;
        }
    }
    if (!data_ok) {
        fprintf(stderr, "[fi]   data integrity FAILED after odd-seq recovery\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   data integrity OK after odd-seq recovery\n");

    /* Step 7: 验证恢复后可正常写入 */
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        indurtdb_write_int32(i, (int32_t)(i + 5000));
    }
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i + 5000)) {
            data_ok = 0;
        }
    }
    if (!data_ok) {
        fprintf(stderr, "[fi]   post-recovery write FAILED\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   post-recovery write/read OK\n");

    indurtdb_shutdown();
    return 0;
}

/* ============================================================
 * 场景 3: SIGKILL 运行时 owner (活跃崩溃)
 *
 * 覆盖代码路径: 与场景 1 相同, 但 owner 在运行时被 SIGKILL
 * (而非 _exit), 更真实地模拟生产环境崩溃.
 * ============================================================ */
static int test_sigkill_active_owner(void) {
    printf("\n[fi] === Scenario 3: SIGKILL active owner ===\n");

    pid_t owner = fork();
    if (owner < 0) {
        perror("[fi] fork");
        return 1;
    }

    if (owner == 0) {
        /* ---- Owner child: 初始化 + 写入 + 无限循环等待被 kill ---- */
        if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
            _exit(1);
        }
        for (uint32_t i = 0; i < NUM_POINTS; i++) {
            indurtdb_write_int32(i, (int32_t)(i * 300 + 11));
        }
        /* 不调用 shutdown, 不调用 _exit.
         * 保持存活, 等待 supervisor 发送 SIGKILL. */
        while (1) {
            usleep(1000000);  /* 1s */
        }
    }

    /* ---- Supervisor ---- */
    usleep(500000);  /* 500ms: 给 owner 时间完成写入 */

    printf("[fi]   sending SIGKILL to active owner (pid=%d)...\n",
           (int)owner);
    kill(owner, SIGKILL);

    /* 等待 owner 死亡 */
    if (wait_owner(owner) != 0) {
        fprintf(stderr, "[fi]   waitpid error\n");
        return 1;
    }
    printf("[fi]   owner killed\n");

    /* supervisor 初始化 → 崩溃恢复 */
    if (indurtdb_initialize(INSTANCE_ID, MAX_POINTS, MAX_SUBS) != 0) {
        fprintf(stderr, "[fi]   supervisor initialize failed: %s\n",
                indurtdb_get_last_error());
        return 1;
    }
    printf("[fi]   supervisor recovered ownership\n");

    /* 验证 SIGKILL 前写入的数据 */
    int data_ok = 1;
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i * 300 + 11)) {
            fprintf(stderr, "[fi]   int32[%u] mismatch: got %d, want %d\n",
                    i, ri, (int)(i * 300 + 11));
            data_ok = 0;
        }
    }
    if (!data_ok) {
        fprintf(stderr, "[fi]   SIGKILL recovery data FAILED\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   data intact after SIGKILL\n");

    /* 验证可继续写入 */
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        indurtdb_write_int32(i, (int32_t)(i + 9000));
    }
    for (uint32_t i = 0; i < NUM_POINTS; i++) {
        int32_t ri;
        if (indurtdb_read_int32(i, &ri) != 0
            || ri != (int32_t)(i + 9000)) {
            data_ok = 0;
        }
    }
    if (!data_ok) {
        fprintf(stderr, "[fi]   post-SIGKILL write FAILED\n");
        indurtdb_shutdown();
        return 1;
    }
    printf("[fi]   post-SIGKILL write/read OK\n");

    indurtdb_shutdown();
    return 0;
}

/* ============================================================
 * main: 依次运行三个场景
 * ============================================================ */
int main(void) {
    printf("[fi] InduRTDB fault injection test (crash recovery)\n");

    int rc;

    /* 场景 1: owner _exit → 崩溃恢复 */
    rc = test_kill_owner();
    if (rc != 0) {
        printf("[fi] Scenario 1 (kill-owner): FAIL\n");
        return rc;
    }
    printf("[fi] Scenario 1 (kill-owner): PASS\n");

    /* 场景 2: 奇数 write_seq 恢复 */
    rc = test_odd_write_seq();
    if (rc != 0) {
        printf("[fi] Scenario 2 (odd write_seq): FAIL\n");
        return rc;
    }
    printf("[fi] Scenario 2 (odd write_seq): PASS\n");

    /* 场景 3: SIGKILL 活跃 owner */
    rc = test_sigkill_active_owner();
    if (rc != 0) {
        printf("[fi] Scenario 3 (SIGKILL active): FAIL\n");
        return rc;
    }
    printf("[fi] Scenario 3 (SIGKILL active): PASS\n");

    printf("\n[fi] ALL SCENARIOS PASS\n");
    return 0;
}
