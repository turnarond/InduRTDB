/**
 * @file test_c_point_fields.cpp
 * @brief v3.3 T5：source_timestamp_ns 与 quality 质量码（TDD 红阶段）
 *
 * 验收点：
 *   - source_timestamp_ns 落在 offset 112，sizeof 与既有偏移不变（ABI 仍 v1）
 *   - 未提供源时间戳时为 0（退化为旧行为）
 *   - 带源时间戳写入可正确读写
 *   - COMM_FAILURE 语义与 quality 分层宏正确
 */
#include <gtest/gtest.h>

#include <cstddef>
#include <cstring>

extern "C" {
#include <indurtdb/indurtdb.h>

#include "internal/irt_types.h"
}

class PointFieldsTest : public ::testing::Test {
protected:
    void SetUp() override
    {
        ASSERT_EQ(indurtdb_initialize("pf_test_fields", 32, 4), 0);
    }
    void TearDown() override
    {
        indurtdb_shutdown();
    }
};

/* 1. 字段布局 */
TEST_F(PointFieldsTest, SourceTimestampOffsetIs112)
{
    EXPECT_EQ(offsetof(indurtdb_point_t, source_timestamp_ns), 112u);
}

TEST_F(PointFieldsTest, SizeStill128AndOffsetsUnchanged)
{
    EXPECT_EQ(sizeof(indurtdb_point_t), 128u);
    EXPECT_EQ(offsetof(indurtdb_point_t, value), 0u);
    EXPECT_EQ(offsetof(indurtdb_point_t, timestamp_ns), 32u);
    EXPECT_EQ(offsetof(indurtdb_point_t, type), 40u);
    EXPECT_EQ(offsetof(indurtdb_point_t, quality), 41u);
    EXPECT_EQ(offsetof(indurtdb_point_t, unit), 42u);
    EXPECT_EQ(offsetof(indurtdb_point_t, access), 44u);
    EXPECT_EQ(offsetof(indurtdb_point_t, name), 45u);
}

/* 2. 未提供源时间戳 → 0（兼容旧行为） */
TEST_F(PointFieldsTest, ZeroSourceTimestampMeansUnprovided)
{
    ASSERT_EQ(indurtdb_write_double(1, 3.14), 0);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(1, &p), 0);
    EXPECT_EQ(p.source_timestamp_ns, 0u);
    EXPECT_EQ(p.value.d, 3.14);
}

/* 3. 带采集时刻写入 */
TEST_F(PointFieldsTest, WriteWithSourceTimestamp)
{
    ASSERT_EQ(indurtdb_write_double_ts(2, 25.5, 1234567890ull), 0);

    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(2, &p), 0);
    EXPECT_EQ(p.source_timestamp_ns, 1234567890ull);
    EXPECT_EQ(p.value.d, 25.5);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
    EXPECT_GT(p.timestamp_ns, 0u); /* 入库时刻仍独立记录 */
}

/* 4. COMM_FAILURE 语义 */
TEST_F(PointFieldsTest, CommFailureSemantics)
{
    ASSERT_EQ(indurtdb_write_int32(3, 7), 0);
    ASSERT_EQ(indurtdb_set_quality(3, INDURTDB_QUALITY_COMM_FAILURE), 0);

    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(3, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_COMM_FAILURE);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(p.quality), INDURTDB_LIMIT_NONE);
    EXPECT_NE(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_GOOD);
    EXPECT_EQ(p.value.i, 7); /* 值本身不变 */
}

/* 5. quality 分层宏 */
TEST_F(PointFieldsTest, QualityMacros)
{
    uint8_t q_high = INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_GOOD, INDURTDB_LIMIT_HIGH);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(q_high), INDURTDB_QUALITY_GOOD);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(q_high), INDURTDB_LIMIT_HIGH);

    uint8_t q_timeout_const = INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_TIMEOUT, INDURTDB_LIMIT_CONSTANT);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(q_timeout_const), INDURTDB_QUALITY_TIMEOUT);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(q_timeout_const), INDURTDB_LIMIT_CONSTANT);
}
