/**
 * @file test_c_layout_seqlock.cpp
 * @brief C 版布局与 seqlock 单元测试
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <thread>

extern "C" {
#include "internal/irt_types.h"
#include "internal/irt_seqlock.h"
}

TEST(CLayout, SizesMatchV2) {
    EXPECT_EQ(sizeof(irt_header_t), 64u);
    EXPECT_EQ(sizeof(indurtdb_point_t), 128u);
    EXPECT_EQ(sizeof(irt_subscriber_entry_t), 16u);
    EXPECT_EQ(IRT_MAGIC, 0x1DBA1DBAu);
    EXPECT_EQ(IRT_SHM_VERSION, 1u);
}

TEST(CLayout, PointFieldOffsets) {
    /* 与 v2.x PointData 字段偏移一致: value(0) ts(32) type(40) quality(41)
       unit(42) access(44) name(45) padding(109) */
    EXPECT_EQ(offsetof(indurtdb_point_t, value), 0u);
    EXPECT_EQ(offsetof(indurtdb_point_t, timestamp_ns), 32u);
    EXPECT_EQ(offsetof(indurtdb_point_t, type), 40u);
    EXPECT_EQ(offsetof(indurtdb_point_t, quality), 41u);
    EXPECT_EQ(offsetof(indurtdb_point_t, unit), 42u);
    EXPECT_EQ(offsetof(indurtdb_point_t, access), 44u);
    EXPECT_EQ(offsetof(indurtdb_point_t, name), 45u);
}

class CSeqlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&header_, 0, sizeof(header_));
        std::memset(points_, 0, sizeof(points_));
    }
    irt_header_t     header_;
    indurtdb_point_t points_[4];
};

TEST_F(CSeqlockTest, WriteBeginEnd) {
    uint64_t s0 = irt_seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s0 & 1ULL, 0ULL);
    EXPECT_EQ(header_.write_seq, s0 + 1);
    irt_seqlock_write_end(&header_.write_seq, s0);
    EXPECT_EQ(header_.write_seq, s0 + 2);
}

TEST_F(CSeqlockTest, WriteConflictDetection) {
    uint64_t s0 = irt_seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s0 & 1ULL, 0ULL);
    /* 第二个 writer 在第一个未结束时应看到奇数 */
    uint64_t s1 = irt_seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s1 & 1ULL, 1ULL);
    irt_seqlock_write_end(&header_.write_seq, s0);
}

TEST_F(CSeqlockTest, ReadReturnsConsistentData) {
    points_[2].value.i = 42;
    /* 标准 seqlock 读模式: 在重试循环内读取数据, 避免 TOCTOU 脏读 */
    int32_t val;
    uint64_t s0, s1;
    do {
        s0 = __atomic_load_n(&header_.write_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;
        val = points_[2].value.i;
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(&header_.write_seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    EXPECT_EQ(val, 42);
}
