/**
 * @file smoke_multi_process.cpp
 * @brief InduRTDB 多进程共享内存冒烟测试
 * @version 1.0.0
 * @date 2026-09-17
 * @copyright MIT License
 *
 * 验证共享内存在 fork 后的多进程间正确共享：
 *   父进程 init + write → fork 子进程 peek/read → 子进程退出 →
 *   父进程验证 write_count → shutdown
 *
 * 不依赖 GTest，独立可执行文件，返回 0 表示全部通过，非 0 表示失败。
 */

#include <indurtdb.hpp>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <sys/wait.h>

namespace {

int g_failures = 0;

void check(bool condition, const char* msg) {
    if (condition) {
        std::printf("  [PASS] %s\n", msg);
    } else {
        std::printf("  [FAIL] %s\n", msg);
        ++g_failures;
    }
}

} // namespace

int main() {
    std::printf("=== InduRTDB 多进程共享内存冒烟测试 ===\n\n");

    auto& rtdb = indurtdb::InduRTDB::instance();

    // ---------- 阶段 1: 父进程初始化 + 写入 ----------
    std::printf("[1] 父进程 (PID=%d) 初始化并写入数据\n", getpid());

    if (!rtdb.initialize("smoke_multi_proc", 4096, 16)) {
        std::printf("[FATAL] 父进程初始化失败，冒烟测试中止\n");
        return 1;
    }
    std::printf("  [PASS] 父进程 initialize() 成功\n");

    check(rtdb.write(1, 23.5),          "父进程 write(1, 23.5)");
    check(rtdb.write(2, (int32_t)42),   "父进程 write(2, 42)");
    check(rtdb.write(3, true),          "父进程 write(3, true)");
    check(rtdb.write(4, "shared_data"), "父进程 write(4, \"shared_data\")");

    // ---------- 阶段 2: fork 子进程读取共享内存 ----------
    std::printf("\n[2] fork 子进程读取共享内存\n");

    pid_t pid = fork();
    if (pid < 0) {
        std::printf("[FATAL] fork 失败\n");
        rtdb.shutdown();
        return 1;
    }

    if (pid == 0) {
        // ===== 子进程 =====
        // 子进程不调用 initialize，直接使用继承的共享内存映射
        int child_failures = 0;

        const indurtdb::PointData* p1 = rtdb.peek(1);
        if (p1 && p1->value.d == 23.5) {
            std::printf("  [PASS] 子进程 peek(1) == 23.5\n");
        } else {
            std::printf("  [FAIL] 子进程 peek(1) 期望值 23.5\n");
            ++child_failures;
        }

        const indurtdb::PointData* p2 = rtdb.peek(2);
        if (p2 && p2->value.i == 42) {
            std::printf("  [PASS] 子进程 peek(2) == 42\n");
        } else {
            std::printf("  [FAIL] 子进程 peek(2) 期望值 42\n");
            ++child_failures;
        }

        const indurtdb::PointData* p3 = rtdb.peek(3);
        if (p3 && p3->value.b == true) {
            std::printf("  [PASS] 子进程 peek(3) == true\n");
        } else {
            std::printf("  [FAIL] 子进程 peek(3) 期望值 true\n");
            ++child_failures;
        }

        indurtdb::PointData p4;
        if (rtdb.read(4, p4) && std::strcmp(p4.value.str, "shared_data") == 0) {
            std::printf("  [PASS] 子进程 read(4) == \"shared_data\"\n");
        } else {
            std::printf("  [FAIL] 子进程 read(4) 期望值 \"shared_data\"\n");
            ++child_failures;
        }

        // 子进程必须使用 _exit()，避免继承的析构函数调用 shm_unlink
        fflush(stdout);
        fflush(stderr);
        _exit(child_failures == 0 ? 0 : 1);
    }

    // ===== 父进程等待子进程 =====
    int status = 0;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        int child_rc = WEXITSTATUS(status);
        check(child_rc == 0, "子进程退出码 == 0 (全部断言通过)");
    } else {
        check(false, "子进程异常终止");
    }

    // ---------- 阶段 3: 父进程验证统计 ----------
    std::printf("\n[3] 父进程验证写入统计\n");
    uint64_t count = rtdb.get_write_count();
    check(count == 4, "get_write_count() == 4 (父进程写入 4 个点位)");

    // ---------- 阶段 4: 关闭 ----------
    std::printf("\n[4] 父进程关闭数据库\n");
    rtdb.shutdown();
    std::printf("  [INFO] shutdown() 已调用\n");

    // ---------- 结果汇总 ----------
    std::printf("\n========================================\n");
    if (g_failures == 0) {
        std::printf("多进程冒烟测试结果: 全部通过 (PASS)\n");
        std::printf("========================================\n");
        return 0;
    } else {
        std::printf("多进程冒烟测试结果: %d 项失败 (FAIL)\n", g_failures);
        std::printf("========================================\n");
        return 1;
    }
}
