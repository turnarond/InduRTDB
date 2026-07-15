/**
 * @file multi_process_example.cpp
 * @brief InduRTDB 多进程共享内存示例
 * @version 1.0.0
 * @date 2026-05-17
 * @copyright MIT License
 */

#include <indurtdb.hpp>
#include <cstdio>
#include <unistd.h>
#include <sys/wait.h>

int main() {
    auto& rtdb = indurtdb::InduRTDB::instance();

    // --- 阶段 1: 父进程初始化 + 写数据 ---
    printf("=== InduRTDB 多进程共享内存示例 ===\n\n");
    printf("[父进程 PID=%d] 初始化共享内存...\n", getpid());

    if (!rtdb.initialize("multi_proc", 4096, 10)) {
        fprintf(stderr, "[父进程] 初始化失败\n");
        return 1;
    }
    printf("[父进程] ok 初始化成功\n\n");

    printf("[父进程] 写入数据...\n");
    rtdb.write(1, 23.5);
    rtdb.write(2, (int32_t)42);
    rtdb.write(3, true);
    rtdb.write(4, "shared_data");
    printf("[父进程] ok 已写入 4 个点位\n\n");

    // --- 阶段 2: fork 子进程读取 ---
    printf("[父进程] fork 子进程...\n");
    pid_t pid = fork();

    if (pid < 0) {
        fprintf(stderr, "[父进程] fork 失败\n");
        return 1;
    }

    if (pid == 0) {
        // ===== 子进程 =====
        printf("[子进程 PID=%d] 启动，读取共享内存...\n", getpid());

        const indurtdb::PointData* p1 = rtdb.peek(1);
        const indurtdb::PointData* p2 = rtdb.peek(2);
        const indurtdb::PointData* p3 = rtdb.peek(3);

        if (p1) printf("[子进程] peek(1) = %.1f\n", p1->value.d);
        if (p2) printf("[子进程] peek(2) = %d\n", p2->value.i);
        if (p3) printf("[子进程] peek(3) = %s\n", p3->value.b ? "true" : "false");

        indurtdb::PointData p4;
        if (rtdb.read(4, p4)) {
            printf("[子进程] read(4) = \"%s\" (type=%d, quality=%d)\n",
                   p4.value.str, (int)p4.type, (int)p4.quality);
        }

        // 子进程退出前必须刷新缓冲区（_exit 不会自动 flush）
        fflush(stdout);
        fflush(stderr);
        // 重要: 必须 _exit() 而非 exit()
        _exit(0);
    }

    // ===== 父进程等待子进程退出 =====
    int status;
    waitpid(pid, &status, 0);
    printf("\n[父进程] 子进程已退出 (status=%d)\n", WEXITSTATUS(status));

    uint64_t count = rtdb.get_write_count();
    printf("[父进程] 总写入次数: %lu\n", (unsigned long)count);

    rtdb.shutdown();
    printf("[父进程] 已关闭共享内存\n");
    printf("\n=== 示例运行完成 ===\n");
    return 0;
}
