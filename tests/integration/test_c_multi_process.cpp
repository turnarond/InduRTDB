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
#include <cstdio>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* 子进程退出码约定: 0=成功, 1=rc非零, 2=值错误 */
#define CHILD_OK       0
#define CHILD_RC_ERR   1
#define CHILD_VAL_ERR  2

/* ---- 辅助: 检查并清理残留共享内存段 ---- */
static void cleanup_stale_shm(const char* instance_id) {
    char name[128];
    snprintf(name, sizeof(name), "/indurtdb_%s", instance_id);
    shm_unlink(name);  /* 不计成败, 只是尽力清理 */
}

/* 子进程 attach 同名实例并读取父进程写入的值 */
TEST(CMultiProcess, ChildReadsParentWrite) {
    cleanup_stale_shm("mp_test_1");

    ASSERT_EQ(indurtdb_initialize("mp_test_1", 64, 8), 0)
        << "init failed: " << indurtdb_get_last_error();
    ASSERT_EQ(indurtdb_write_int32(5, 12345), 0);

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        /* 子进程: 已继承映射 -- 先测试低级原始读取绕过 seqlock */
        if (!indurtdb_is_initialized()) {
            fprintf(stderr, "[child] indurtdb_is_initialized() returned false\n");
            _exit(CHILD_RC_ERR);
        }

        /* 直接 peek 绕过 seqlock 读取 */
        const indurtdb_point_t* pp = indurtdb_peek(5);
        if (!pp) {
            fprintf(stderr, "[child] peek(5) returned NULL\n");
            _exit(CHILD_RC_ERR);
        }

        /* 校验通过 peek 获取的值 */
        if (pp->value.i != 12345) {
            fprintf(stderr, "[child] peek(5)->value.i = %d, expected 12345\n",
                    pp->value.i);
            _exit(CHILD_VAL_ERR);
        }

        /* 正常 read API */
        int32_t v = 0;
        int rc = indurtdb_read_int32(5, &v);
        if (rc != 0) {
            fprintf(stderr, "[child] read_int32(5) rc=%d, err=%s\n",
                    rc, indurtdb_get_last_error());
            _exit(CHILD_RC_ERR);
        }
        if (v != 12345) {
            fprintf(stderr, "[child] read_int32(5) = %d, expected 12345\n", v);
            _exit(CHILD_VAL_ERR);
        }
        _exit(CHILD_OK);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    EXPECT_TRUE(WIFEXITED(status))
        << "child did not exit normally (maybe crashed)";
    EXPECT_EQ(WEXITSTATUS(status), CHILD_OK)
        << "child exit code " << WEXITSTATUS(status);
    indurtdb_shutdown();
}

/* 子进程写入, 父进程观察 (共享内存双向) */
TEST(CMultiProcess, ParentSeesChildWrite) {
    cleanup_stale_shm("mp_test_2");

    ASSERT_EQ(indurtdb_initialize("mp_test_2", 64, 8), 0)
        << "init failed: " << indurtdb_get_last_error();

    pid_t pid = fork();
    ASSERT_GE(pid, 0);

    if (pid == 0) {
        int rc = indurtdb_write_double(9, 2.718);
        if (rc != 0) {
            fprintf(stderr, "[child] write_double(9) rc=%d\n", rc);
            _exit(CHILD_RC_ERR);
        }
        _exit(CHILD_OK);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    ASSERT_EQ(WEXITSTATUS(status), CHILD_OK)
        << "child write failed, exit=" << WEXITSTATUS(status);

    double v = 0;
    EXPECT_EQ(indurtdb_read_double(9, &v), 0);
    EXPECT_DOUBLE_EQ(v, 2.718);
    indurtdb_shutdown();
}

/* 布局回归: 写入后从原始共享内存字节直接校验 v2.x 布局 */
TEST(CMultiProcess, RawLayoutRegression) {
    cleanup_stale_shm("mp_test_3");

    ASSERT_EQ(indurtdb_initialize("mp_test_3", 4, 2), 0)
        << "init failed: " << indurtdb_get_last_error();
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
