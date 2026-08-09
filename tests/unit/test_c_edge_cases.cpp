/**
 * @file test_c_edge_cases.cpp
 * @brief 边界/异常/质量路径测试: 越界 id、字符串截断、未初始化调用、
 *        重复初始化、超时边界、错误信息、NULL 指针、批量写越界
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <string>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* 每个 test 独立 init/shutdown, 避免单例全局状态交叉污染 */

/* ================================================================
 * 1. 越界 ID: write / read / peek 拒绝
 * ================================================================ */

class EdgeIdOutOfRange : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("ec_id_oor", 64, 8), 0);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* EC-01: write_bool 越界返回非零 */
TEST_F(EdgeIdOutOfRange, WriteBoolOutOfRange) {
    EXPECT_NE(indurtdb_write_bool(64, true), 0);
    EXPECT_NE(indurtdb_write_bool(10000, false), 0);
    EXPECT_NE(indurtdb_write_bool(UINT32_MAX, true), 0);
}

/* EC-02: write_int32 越界返回非零 */
TEST_F(EdgeIdOutOfRange, WriteInt32OutOfRange) {
    EXPECT_NE(indurtdb_write_int32(64, 42), 0);
    EXPECT_NE(indurtdb_write_int32(9999, 1), 0);
}

/* EC-03: write_double 越界返回非零 */
TEST_F(EdgeIdOutOfRange, WriteDoubleOutOfRange) {
    EXPECT_NE(indurtdb_write_double(64, 1.0), 0);
    EXPECT_NE(indurtdb_write_double(UINT32_MAX, 0.0), 0);
}

/* EC-04: write_string 越界返回非零 */
TEST_F(EdgeIdOutOfRange, WriteStringOutOfRange) {
    EXPECT_NE(indurtdb_write_string(64, "test"), 0);
    EXPECT_NE(indurtdb_write_string(9999, "x"), 0);
}

/* EC-05: read_bool 越界返回非零 */
TEST_F(EdgeIdOutOfRange, ReadBoolOutOfRange) {
    bool b = false;
    EXPECT_NE(indurtdb_read_bool(64, &b), 0);
}

/* EC-06: read_int32 越界返回非零 */
TEST_F(EdgeIdOutOfRange, ReadInt32OutOfRange) {
    int32_t v = 0;
    EXPECT_NE(indurtdb_read_int32(64, &v), 0);
    EXPECT_NE(indurtdb_read_int32(10000, &v), 0);
}

/* EC-07: read_double 越界返回非零 */
TEST_F(EdgeIdOutOfRange, ReadDoubleOutOfRange) {
    double d = 0;
    EXPECT_NE(indurtdb_read_double(64, &d), 0);
}

/* EC-08: read_string 越界返回非零 */
TEST_F(EdgeIdOutOfRange, ReadStringOutOfRange) {
    char buf[32];
    EXPECT_NE(indurtdb_read_string(64, buf, sizeof(buf)), 0);
}

/* EC-09: read_point 越界返回非零 */
TEST_F(EdgeIdOutOfRange, ReadPointOutOfRange) {
    indurtdb_point_t pt;
    EXPECT_NE(indurtdb_read_point(64, &pt), 0);
    EXPECT_NE(indurtdb_read_point(9999, &pt), 0);
}

/* EC-10: peek 越界返回 NULL */
TEST_F(EdgeIdOutOfRange, PeekOutOfRange) {
    EXPECT_EQ(indurtdb_peek(64), nullptr);
    EXPECT_EQ(indurtdb_peek(UINT32_MAX), nullptr);
}

/* EC-11: max_points 边界: id=63 (max-1) 成功, id=64 (max) 失败 */
TEST_F(EdgeIdOutOfRange, BoundaryExactMaxPoints) {
    EXPECT_EQ(indurtdb_write_int32(63, 123), 0);
    int32_t v = 0;
    EXPECT_EQ(indurtdb_read_int32(63, &v), 0);
    EXPECT_EQ(v, 123);

    EXPECT_NE(indurtdb_write_int32(64, 456), 0);
    EXPECT_NE(indurtdb_read_int32(64, &v), 0);
    EXPECT_EQ(indurtdb_peek(64), nullptr);
}

/* ================================================================
 * 2. 字符串长度限制与截断
 * ================================================================ */

class EdgeString : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("ec_str", 64, 8), 0);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* EC-12: 31 字符恰好存入, 不截断 */
TEST_F(EdgeString, Exactly31CharsFits) {
    /* 31 个 'a' + '\0' = 32 字节, 恰好填满 value.str[32] */
    std::string s(31, 'a');
    ASSERT_EQ(indurtdb_write_string(0, s.c_str()), 0);

    char buf[64] = {};
    ASSERT_EQ(indurtdb_read_string(0, buf, sizeof(buf)), 0);
    EXPECT_EQ(strlen(buf), 31u);
    EXPECT_STREQ(buf, s.c_str());
}

/* EC-13: 32+ 字符被截断为 31 字符 */
TEST_F(EdgeString, TruncatedAt31) {
    /* strncpy(str, value, 31); str[31]='\0' → 最多 31 有效字符 */
    std::string s(50, 'x');
    ASSERT_EQ(indurtdb_write_string(1, s.c_str()), 0);

    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(1, &pt), 0);
    EXPECT_EQ(strlen(pt.value.str), 31u);
    EXPECT_EQ(pt.value.str[31], '\0');

    char buf[64] = {};
    ASSERT_EQ(indurtdb_read_string(1, buf, sizeof(buf)), 0);
    EXPECT_EQ(strlen(buf), 31u);
}

/* EC-14: 空字符串正常写入 */
TEST_F(EdgeString, EmptyString) {
    ASSERT_EQ(indurtdb_write_string(2, ""), 0);
    char buf[32] = "non-empty";
    ASSERT_EQ(indurtdb_read_string(2, buf, sizeof(buf)), 0);
    EXPECT_STREQ(buf, "");
}

/* EC-15: 恰好 32 字符 (含 \0 需 33 字节), 截断为 31 */
TEST_F(EdgeString, Exactly32CharsTruncated) {
    std::string s(32, 'b');
    ASSERT_EQ(indurtdb_write_string(3, s.c_str()), 0);

    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(3, &pt), 0);
    EXPECT_EQ(strlen(pt.value.str), 31u);
}

/* ================================================================
 * 3. 未初始化调用行为 (ENSURE_INIT 守卫)
 * ================================================================ */

class EdgeUninitialized : public ::testing::Test {
protected:
    void SetUp() override {
        /* 确保未初始化状态 */
        indurtdb_shutdown();
        ASSERT_FALSE(indurtdb_is_initialized());
    }
    void TearDown() override {
        /* 清理: 确保干净状态留给后续测试 */
        indurtdb_shutdown();
    }
};

/* EC-16: 所有 write_* 返回 -1, last_error 为 "not initialized" */
TEST_F(EdgeUninitialized, WriteReturnsError) {
    EXPECT_EQ(indurtdb_write_bool(0, true), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    EXPECT_EQ(indurtdb_write_int32(0, 1), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    EXPECT_EQ(indurtdb_write_double(0, 1.0), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    EXPECT_EQ(indurtdb_write_string(0, "x"), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-17: 所有 read_* 返回 -1 (null ptr 检查优先于 init 检查) */
TEST_F(EdgeUninitialized, ReadReturnsError) {
    bool b;
    EXPECT_EQ(indurtdb_read_bool(0, &b), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    int32_t i;
    EXPECT_EQ(indurtdb_read_int32(0, &i), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    double d;
    EXPECT_EQ(indurtdb_read_double(0, &d), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    char buf[32];
    EXPECT_EQ(indurtdb_read_string(0, buf, sizeof(buf)), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    indurtdb_point_t pt;
    EXPECT_EQ(indurtdb_read_point(0, &pt), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-18: peek 返回 NULL, last_error 为 "not initialized" */
TEST_F(EdgeUninitialized, PeekReturnsNull) {
    EXPECT_EQ(indurtdb_peek(0), nullptr);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-19: check_timeouts 返回 -1 */
TEST_F(EdgeUninitialized, CheckTimeoutsReturnsError) {
    EXPECT_EQ(indurtdb_check_timeouts(1000), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-20: validate_id 返回 0 (合法/非法 id 均返回 0) */
TEST_F(EdgeUninitialized, ValidateIdReturnsZero) {
    EXPECT_EQ(indurtdb_validate_id(0), 0);
    EXPECT_EQ(indurtdb_validate_id(9999), 0);
}

/* EC-21: get_write_count / get_timeout_count 返回 0 */
TEST_F(EdgeUninitialized, CountersReturnZero) {
    EXPECT_EQ(indurtdb_get_write_count(), 0u);
    EXPECT_EQ(indurtdb_get_timeout_count(), 0u);
}

/* EC-22: subscribe/unsubscribe 返回 -1 */
TEST_F(EdgeUninitialized, SubscribeReturnsError) {
    auto cb = [](uint32_t, const indurtdb_point_t*, void*) {};
    EXPECT_EQ(indurtdb_subscribe(0, cb, nullptr), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    EXPECT_EQ(indurtdb_unsubscribe(0), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-23: 批量读写返回 -1 */
TEST_F(EdgeUninitialized, BatchOpsReturnError) {
    indurtdb_point_t buf[4];
    EXPECT_EQ(indurtdb_read_range(0, 4, buf, 4), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    bool bvals[] = {true, false};
    EXPECT_EQ(indurtdb_write_range_bool(0, bvals, 2), -1);

    int32_t ivals[] = {1, 2};
    EXPECT_EQ(indurtdb_write_range_int32(0, ivals, 2), -1);

    double dvals[] = {1.0, 2.0};
    EXPECT_EQ(indurtdb_write_range_double(0, dvals, 2), -1);
}

/* EC-24: load_config 返回 -1 */
TEST_F(EdgeUninitialized, LoadConfigReturnsError) {
    EXPECT_EQ(indurtdb_load_config("/tmp/nonexistent.yaml"), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}

/* EC-25: update_heartbeat 不崩溃 (无返回值) */
TEST_F(EdgeUninitialized, UpdateHeartbeatNoCrash) {
    EXPECT_NO_FATAL_FAILURE(indurtdb_update_heartbeat());
}

/* ================================================================
 * 4. 重复初始化
 * ================================================================ */

/* EC-26: 二次 initialize 返回 -1, last_error = "already initialized" */
TEST(EdgeDoubleInit, ReturnsError) {
    ASSERT_EQ(indurtdb_initialize("ec_dbl_init", 64, 8), 0);
    EXPECT_TRUE(indurtdb_is_initialized());

    EXPECT_EQ(indurtdb_initialize("ec_dbl_init2", 64, 8), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "already initialized");

    /* 第一次初始化仍有效 */
    EXPECT_TRUE(indurtdb_is_initialized());
    EXPECT_EQ(indurtdb_write_int32(0, 42), 0);

    indurtdb_shutdown();
}

/* EC-27: NULL instance_id 返回 "invalid argument" */
TEST(EdgeInitArgs, NullInstanceId) {
    EXPECT_NE(indurtdb_initialize(nullptr, 64, 8), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

/* EC-28: 空字符串 instance_id 返回 "invalid argument" */
TEST(EdgeInitArgs, EmptyInstanceId) {
    EXPECT_NE(indurtdb_initialize("", 64, 8), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

/* EC-29: max_points=0 返回 "invalid argument" */
TEST(EdgeInitArgs, ZeroMaxPoints) {
    EXPECT_NE(indurtdb_initialize("ec_zero_mp", 0, 8), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

/* ================================================================
 * 5. check_timeouts 边界
 * ================================================================ */

class EdgeTimeout : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("ec_timeout", 64, 8), 0);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* EC-30: timeout_ns=0 → 不扫描, 返回 0 */
TEST_F(EdgeTimeout, ZeroTimeoutNoScan) {
    ASSERT_EQ(indurtdb_write_int32(0, 1), 0);
    EXPECT_EQ(indurtdb_check_timeouts(0), 0);

    /* 点位 quality 仍为 GOOD */
    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(0, &pt), 0);
    EXPECT_EQ(pt.quality, INDURTDB_QUALITY_GOOD);
}

/* EC-31: 从未写入的点 (timestamp_ns=0) 不被标记 TIMEOUT */
TEST_F(EdgeTimeout, NeverWrittenPointSkipped) {
    /* 只写 id=0, 不写 id=5 */
    ASSERT_EQ(indurtdb_write_int32(0, 1), 0);

    /* timeout_ns=1: 极小超时, id=0 应被标记, id=5 (未写入) 不应被标记 */
    indurtdb_check_timeouts(1);

    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(5, &pt), 0);
    EXPECT_EQ(pt.timestamp_ns, 0u);
    EXPECT_NE(pt.quality, INDURTDB_QUALITY_TIMEOUT);
}

/* EC-32: 已是 TIMEOUT 的点不重复计数 */
TEST_F(EdgeTimeout, AlreadyTimeoutNotRecounted) {
    ASSERT_EQ(indurtdb_write_int32(10, 100), 0);

    int first = indurtdb_check_timeouts(1);
    EXPECT_GE(first, 1);

    /* 第二次: 已是 TIMEOUT 的点被跳过 */
    int second = indurtdb_check_timeouts(1);
    EXPECT_EQ(second, 0);
}

/* EC-33: 重新写入后 quality 从 TIMEOUT 恢复为 GOOD */
TEST_F(EdgeTimeout, WriteAfterTimeoutRestoresGood) {
    ASSERT_EQ(indurtdb_write_int32(20, 1), 0);
    indurtdb_check_timeouts(1);

    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(20, &pt), 0);
    EXPECT_EQ(pt.quality, INDURTDB_QUALITY_TIMEOUT);

    /* 重新写入恢复 */
    ASSERT_EQ(indurtdb_write_int32(20, 2), 0);
    ASSERT_EQ(indurtdb_read_point(20, &pt), 0);
    EXPECT_EQ(pt.quality, INDURTDB_QUALITY_GOOD);
    EXPECT_EQ(pt.value.i, 2);
}

/* EC-34: 极大超时值不标记任何点 */
TEST_F(EdgeTimeout, LargeTimeoutMarksNothing) {
    ASSERT_EQ(indurtdb_write_int32(0, 1), 0);
    ASSERT_EQ(indurtdb_write_int32(1, 2), 0);

    /* UINT64_MAX 超时: now - ts <= UINT64_MAX 恒成立, 无点被标记 */
    int detected = indurtdb_check_timeouts(UINT64_MAX);
    EXPECT_EQ(detected, 0);

    indurtdb_point_t pt;
    ASSERT_EQ(indurtdb_read_point(0, &pt), 0);
    EXPECT_EQ(pt.quality, INDURTDB_QUALITY_GOOD);
}

/* ================================================================
 * 6. get_last_error 行为
 * ================================================================ */

/* EC-35: 返回值始终非 NULL (_Thread_local 静态数组, 永不为 NULL).
 * 注: g_last_error 是 _Thread_local, 同一线程内跨测试持久保留;
 *     首次启动时为 "", 但同线程先前测试设置的错误会保留. */
TEST(EdgeLastError, AlwaysNonNull) {
    const char* err = indurtdb_get_last_error();
    ASSERT_NE(err, nullptr);
    /* 返回可读的 C 字符串 */
    EXPECT_GE(strlen(err), 0u);
}

/* EC-36: 错误后返回可读字符串 */
TEST(EdgeLastError, ReadableAfterError) {
    /* 触发 not-initialized 错误 */
    EXPECT_NE(indurtdb_write_int32(0, 1), 0);
    const char* err = indurtdb_get_last_error();
    ASSERT_NE(err, nullptr);
    EXPECT_GT(strlen(err), 0u);
    EXPECT_STREQ(err, "not initialized");
}

/* EC-37: 成功操作不覆盖已有错误信息 */
TEST(EdgeLastError, SuccessDoesNotClearError) {
    /* 触发错误 */
    EXPECT_NE(indurtdb_write_bool(0, true), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    /* 成功初始化 */
    ASSERT_EQ(indurtdb_initialize("ec_err_keep", 64, 8), 0);

    /* 成功写入 — 不调用 set_error, 旧错误仍在 */
    ASSERT_EQ(indurtdb_write_int32(0, 42), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");

    indurtdb_shutdown();
}

/* ================================================================
 * 7. NULL 指针参数
 * ================================================================ */

class EdgeNullPtr : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("ec_null", 64, 8), 0);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* EC-38: read_* 的 NULL 输出指针返回 -1, "null output pointer" */
TEST_F(EdgeNullPtr, ReadNullOutputPointer) {
    EXPECT_NE(indurtdb_read_bool(0, nullptr), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");

    EXPECT_NE(indurtdb_read_int32(0, nullptr), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");

    EXPECT_NE(indurtdb_read_double(0, nullptr), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");

    EXPECT_NE(indurtdb_read_point(0, nullptr), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null output pointer");
}

/* EC-39: read_string NULL buffer 返回 -1 */
TEST_F(EdgeNullPtr, ReadStringNullBuffer) {
    EXPECT_NE(indurtdb_read_string(0, nullptr, 32), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null or zero-size buffer");
}

/* EC-40: read_string buffer_size=0 返回 -1 */
TEST_F(EdgeNullPtr, ReadStringZeroSize) {
    char buf[8];
    EXPECT_NE(indurtdb_read_string(0, buf, 0), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null or zero-size buffer");
}

/* EC-41: read_range NULL buffer 返回 -1 */
TEST_F(EdgeNullPtr, ReadRangeNullBuffer) {
    EXPECT_NE(indurtdb_read_range(0, 1, nullptr, 1), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

/* EC-42: read_range count=0 返回 -1 */
TEST_F(EdgeNullPtr, ReadRangeZeroCount) {
    indurtdb_point_t pt;
    EXPECT_NE(indurtdb_read_range(0, 0, &pt, 1), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "invalid argument");
}

/* EC-43: 批量写 NULL values 指针返回 -1 */
TEST_F(EdgeNullPtr, BatchWriteNullValues) {
    EXPECT_EQ(indurtdb_write_range_bool(0, nullptr, 3), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "null values pointer");

    EXPECT_EQ(indurtdb_write_range_int32(0, nullptr, 3), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "null values pointer");

    EXPECT_EQ(indurtdb_write_range_double(0, nullptr, 3), -1);
    EXPECT_STREQ(indurtdb_get_last_error(), "null values pointer");
}

/* EC-44: load_config NULL path 返回 -1 */
TEST_F(EdgeNullPtr, LoadConfigNullPath) {
    EXPECT_NE(indurtdb_load_config(nullptr), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "null config path");
}

/* ================================================================
 * 8. 批量写越界边界
 * ================================================================ */

class EdgeBatchBoundary : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("ec_batch", 64, 8), 0);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* EC-45: 批量写跨越 max_points 边界, 返回已成功写入的数量 */
TEST_F(EdgeBatchBoundary, WriteRangeCrossesBoundary) {
    /* start_id=63, count=3: id=63 成功, id=64 失败 → 返回 1 */
    int32_t vals[] = {100, 200, 300};
    int n = indurtdb_write_range_int32(63, vals, 3);
    EXPECT_EQ(n, 1);

    /* 验证 id=63 确实写入了 */
    int32_t v = 0;
    EXPECT_EQ(indurtdb_read_int32(63, &v), 0);
    EXPECT_EQ(v, 100);
}

/* EC-46: 批量写全部越界返回 0 */
TEST_F(EdgeBatchBoundary, WriteRangeAllOutOfRange) {
    bool bvals[] = {true, false};
    int n = indurtdb_write_range_bool(64, bvals, 2);
    EXPECT_EQ(n, 0);
}

/* EC-47: read_range 跨越边界返回 -1 */
TEST_F(EdgeBatchBoundary, ReadRangeCrossesBoundary) {
    /* start_id=63, count=2: id=63 可读, id=64 越界 → -1 */
    indurtdb_point_t buf[2];
    int rc = indurtdb_read_range(63, 2, buf, 2);
    EXPECT_EQ(rc, -1);
}

/* EC-48: 批量写 count=0 返回 0 (循环不执行) */
TEST_F(EdgeBatchBoundary, WriteRangeZeroCount) {
    int32_t vals[] = {1};
    int n = indurtdb_write_range_int32(0, vals, 0);
    EXPECT_EQ(n, 0);
}

/* EC-49: 批量写全部成功返回 count */
TEST_F(EdgeBatchBoundary, WriteRangeAllSuccess) {
    double vals[] = {1.1, 2.2, 3.3};
    int n = indurtdb_write_range_double(10, vals, 3);
    EXPECT_EQ(n, 3);

    double d;
    EXPECT_EQ(indurtdb_read_double(10, &d), 0);
    EXPECT_DOUBLE_EQ(d, 1.1);
    EXPECT_EQ(indurtdb_read_double(12, &d), 0);
    EXPECT_DOUBLE_EQ(d, 3.3);
}
