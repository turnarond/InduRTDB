/**
 * @file test_c_multi_process.cpp
 * @brief 多进程共享内存集成测试 (fork + C API)
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <sys/wait.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* 子进程 attach 同名实例并读取父进程写入的值 */
TEST(CMultiProcess, ChildReadsParentWrite) {
    ASSERT_EQ(indurtdb_initialize("mp_test_1", 64, 8), 0);
    ASSERT_EQ(indurtdb_write_int32(5, 12345), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* 子进程: 已继承映射 —— 直接读 (同一 g_ctx 已初始化) */
        int32_t v = 0;
        int rc = indurtdb_read_int32(5, &v);
        _exit(rc == 0 && v == 12345 ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    indurtdb_shutdown();
}

/* 子进程写入, 父进程观察 (共享内存双向) */
TEST(CMultiProcess, ParentSeesChildWrite) {
    ASSERT_EQ(indurtdb_initialize("mp_test_2", 64, 8), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        int rc = indurtdb_write_double(9, 2.718);
        _exit(rc == 0 ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_EQ(WEXITSTATUS(status), 0);

    double v = 0;
    EXPECT_EQ(indurtdb_read_double(9, &v), 0);
    EXPECT_DOUBLE_EQ(v, 2.718);
    indurtdb_shutdown();
}

/* 实例隔离: fork 后子进程创建独立实例, 父进程实例不受影响 */
TEST(CMultiProcess, InstanceIsolation) {
    shm_unlink("/indurtdb_test_A");
    shm_unlink("/indurtdb_test_B");

    ASSERT_EQ(indurtdb_initialize("test_A", 32, 2), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* 子进程: 创建独立实例 test_B (fork 检测自动重置 g_rtdb) */
        ASSERT_EQ(indurtdb_initialize("test_B", 64, 2), 0);
        indurtdb_shutdown();
        _exit(0);
    }

    wait(nullptr);

    /* 父进程: test_A 仍可正常读写 */
    ASSERT_EQ(indurtdb_write_int32(10, 123), 0);
    int32_t val = 0;
    ASSERT_EQ(indurtdb_read_int32(10, &val), 0);
    EXPECT_EQ(val, 123);

    indurtdb_shutdown();
}

/* 全类型数据: 父进程写入 4 种类型, fork 后子进程跨进程读取全部 */
TEST(CMultiProcess, AllDataTypes) {
    ASSERT_EQ(indurtdb_initialize("mp_all_types", 64, 8), 0);

    ASSERT_EQ(indurtdb_write_bool(1, true), 0);
    ASSERT_EQ(indurtdb_write_int32(2, -98765), 0);
    ASSERT_EQ(indurtdb_write_double(3, 3.14159), 0);
    ASSERT_EQ(indurtdb_write_string(4, "hello_shm"), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        bool   b = false;
        int32_t i = 0;
        double d = 0.0;
        char   s[64] = "";
        int rc = indurtdb_read_bool(1, &b)
               | indurtdb_read_int32(2, &i)
               | indurtdb_read_double(3, &d)
               | indurtdb_read_string(4, s, sizeof(s));
        bool ok = (rc == 0) && b && (i == -98765)
                  && (d > 3.14 && d < 3.15)
                  && (std::strcmp(s, "hello_shm") == 0);
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    indurtdb_shutdown();
}

/* 零拷贝 peek: 子进程通过 peek 直接访问父进程写入的共享内存 */
TEST(CMultiProcess, ZeroCopyPeek) {
    ASSERT_EQ(indurtdb_initialize("mp_peek", 64, 8), 0);
    ASSERT_EQ(indurtdb_write_double(7, 42.0), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        const indurtdb_point_t* p = indurtdb_peek(7);
        bool ok = (p != NULL) && (p->value.d == 42.0)
                  && (p->type == INDURTDB_TYPE_DOUBLE);
        _exit(ok ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    indurtdb_shutdown();
}

/* 布局回归: 写入后从原始共享内存字节直接校验 v2.x 布局 */
TEST(CMultiProcess, RawLayoutRegression) {
    ASSERT_EQ(indurtdb_initialize("mp_test_3", 4, 2), 0);
    ASSERT_EQ(indurtdb_write_int32(1, 0x11223344), 0);

    const indurtdb_point_t* p = indurtdb_peek(1);
    ASSERT_NE(p, nullptr);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(p);

    /* value.i 位于偏移 0 (小端), type 位于偏移 40 == INT32(1) */
    int32_t vi;
    std::memcpy(&vi, raw + 0, sizeof(vi));
    EXPECT_EQ(vi, 0x11223344);
    EXPECT_EQ(raw[40], INDURTDB_TYPE_INT32);
    EXPECT_EQ(raw[41], INDURTDB_QUALITY_GOOD);

    /* Header 就在 points[0] 前 64 字节处: magic 校验 */
    const uint8_t* base = reinterpret_cast<const uint8_t*>(indurtdb_peek(0)) - 64;
    uint32_t magic;
    std::memcpy(&magic, base, sizeof(magic));
    EXPECT_EQ(magic, 0x1DBA1DBAu);

    indurtdb_shutdown();
}
