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

/* 清理残留共享内存段, 防止跨运行干扰 */
static void cleanup_stale_shm(const char* instance_id) {
    char name[128];
    snprintf(name, sizeof(name), "/indurtdb_%s", instance_id);
    shm_unlink(name);
}

/* 子进程 attach 同名实例并读取父进程写入的值 */
TEST(CMultiProcess, ChildReadsParentWrite) {
    cleanup_stale_shm("mp_test_1");
    ASSERT_EQ(indurtdb_initialize("mp_test_1", 64, 8), 0);
    ASSERT_EQ(indurtdb_write_int32(5, 12345), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* peek(0) 获取基址后直接偏移读取 point 5, 不经过 peek(5)/read_int32 */
        const indurtdb_point_t* p0 = indurtdb_peek(0);
        if (!p0) _exit(1);
        const indurtdb_point_t* p5 =
            (const indurtdb_point_t*)((const uint8_t*)p0
                                      + 5 * sizeof(indurtdb_point_t));
        _exit(p5->value.i == 12345 ? 0 : 1);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    indurtdb_shutdown();
}

/* 子进程写入, 父进程观察 (共享内存双向) */
TEST(CMultiProcess, ParentSeesChildWrite) {
    cleanup_stale_shm("mp_test_2");
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

/* 布局回归: 写入后从原始共享内存字节直接校验 v2.x 布局 */
TEST(CMultiProcess, RawLayoutRegression) {
    cleanup_stale_shm("mp_test_3");
    ASSERT_EQ(indurtdb_initialize("mp_test_3", 4, 2), 0);
    ASSERT_EQ(indurtdb_write_int32(1, 0x11223344), 0);

    const indurtdb_point_t* p = indurtdb_peek(1);
    ASSERT_NE(p, nullptr);
    const uint8_t* raw = reinterpret_cast<const uint8_t*>(p);

    /* value.i 位于偏移 0 (小端), type 位于偏移 40 == INT32(1) */
    int32_t vi;
    std::memcpy(&vi, raw + 0, sizeof(vi));
    EXPECT_EQ(vi, 0x11223344);
    EXPECT_EQ((int)raw[40], INDURTDB_TYPE_INT32);
    EXPECT_EQ((int)raw[41], INDURTDB_QUALITY_GOOD);

    /* Header 就在 points[0] 前 64 字节处: magic 校验 */
    const uint8_t* base = reinterpret_cast<const uint8_t*>(indurtdb_peek(0)) - 64;
    uint32_t magic;
    std::memcpy(&magic, base, sizeof(magic));
    EXPECT_EQ(magic, 0x1DBA1DBAu);

    indurtdb_shutdown();
}
