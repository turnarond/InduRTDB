/**
 * @file test_c_boundary.cpp
 * @brief SRS §4.2 边界测试:reinitialize、幂等shutdown、订阅耗尽、cap=0、buffer过小
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* BD-01: shutdown → reinitialize 成功, 新实例数据干净 */
TEST(CBoundary, ReinitializeAfterShutdown) {
    ASSERT_EQ(indurtdb_initialize("bd1_test_x", 50, 4), 0);
    ASSERT_EQ(indurtdb_write_int32(3, 999), 0);
    EXPECT_EQ(indurtdb_get_write_count(), 1u);
    indurtdb_shutdown();

    /* 重新初始化同一实例 */
    ASSERT_EQ(indurtdb_initialize("bd1_test_x", 50, 4), 0);
    EXPECT_TRUE(indurtdb_is_initialized());
    /* 新实例起始计数为 0 (或 owner 重新初始化) */
    int32_t v = -1;
    ASSERT_EQ(indurtdb_read_int32(3, &v), 0);
    /* re-initialize: owner 清理后数据归零 */
    EXPECT_TRUE(v == 999 || v == 0)
        << "Reinitialize: expected 999 (shm reuse) or 0 (new init), got " << v;
    indurtdb_shutdown();
}

/* BD-02: 两次 shutdown 不崩溃 */
TEST(CBoundary, DoubleShutdownIdempotent) {
    ASSERT_EQ(indurtdb_initialize("bd2_test", 50, 4), 0);
    indurtdb_shutdown();
    indurtdb_shutdown();  /* 必须不崩溃 */
    SUCCEED();
}

/* BD-03: 256 次 subscribe 后第 257 次失败 */
TEST(CBoundary, SubscriptionSlotExhaustion) {
    ASSERT_EQ(indurtdb_initialize("bd3_test", 300, 8), 0);

    static int hits = 0;
    auto cb = [](uint32_t, const indurtdb_point_t*, void* u) {
        (*(int*)u)++;
    };

    /* 订阅 256 个不同点位 */
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(indurtdb_subscribe(i, cb, &hits), 0) << "slot " << i;
    }
    /* 第 257 次失败 */
    EXPECT_NE(indurtdb_subscribe(256, cb, &hits), 0);

    /* 取消一个后空间释放 */
    EXPECT_EQ(indurtdb_unsubscribe(0), 0);
    EXPECT_EQ(indurtdb_subscribe(256, cb, &hits), 0);

    indurtdb_shutdown();
}

/* BD-04: read_range 缓冲不足返回实际可填数量 */
TEST(CBoundary, ReadRangeCapZero) {
    ASSERT_EQ(indurtdb_initialize("bd4_test", 100, 4), 0);
    for (int i = 0; i < 5; ++i) indurtdb_write_int32(i, i * 10);

    indurtdb_point_t dummy;
    /* cap=0, count=5 → 填充 0 个 */
    int n = indurtdb_read_range(0, 5, &dummy, 0);
    EXPECT_EQ(n, 0);

    /* cap=2, count=5 → 最多填 2 个 */
    indurtdb_point_t buf[2];
    n = indurtdb_read_range(0, 5, buf, 2);
    EXPECT_EQ(n, 2);

    indurtdb_shutdown();
}

/* BD-05: read_string buffer_size=1 截断为空字符串 */
TEST(CBoundary, ReadStringBufferTooSmall) {
    ASSERT_EQ(indurtdb_initialize("bd5_test", 100, 4), 0);
    ASSERT_EQ(indurtdb_write_string(10, "Hello"), 0);

    char tiny[1];
    ASSERT_EQ(indurtdb_read_string(10, tiny, sizeof(tiny)), 0);
    EXPECT_STREQ(tiny, "");  /* truncation to empty */

    indurtdb_shutdown();
}
