/**
 * @file test_point_manager_timeout.cpp
 * @brief 超时点位标记单元测试 —— SRS §7.2 `indurtdb_timeouts_total` 指标
 * @version 2.2.0
 *
 * 语义约定（2026-05 决策）：
 *   超时采用「被动标记」—— 库不判断阈值，由驱动层显式调用 mark_timeout()
 *   每次成功标记：quality 置 Quality::TIMEOUT，stats.timeouts 原子递增
 */

#include <gtest/gtest.h>
#include <indurtdb/core/point_manager_interface.hpp>
#include <indurtdb/osal/factory.hpp>
#include <indurtdb/types/memory_layout.hpp>
#include <cstring>

using namespace indurtdb;
using namespace indurtdb::core;

namespace {

constexpr uint32_t kMaxPoints = 8;

// 与 SharedMemorySegment 一致的裸字节布局。
// 刻意不用 struct —— PointData 的 128 字节对齐会让编译器在 Header 之后
// 插入填充，导致与 PointManager 的偏移算法不一致。
class PointManagerTimeoutTest : public ::testing::Test {
protected:
    void SetUp() override {
        time_ = osal::OSALFactory::create_time();
        std::memset(buf_, 0, sizeof(buf_));
        header().max_points = kMaxPoints;
        pm_ = new PointManager(buf_, kMaxPoints, time_.get());
    }

    void TearDown() override {
        delete pm_;
        time_.reset();
    }

    InduRTDBHeader& header() { return *reinterpret_cast<InduRTDBHeader*>(buf_); }
    PointData* points() {
        return reinterpret_cast<PointData*>(buf_ + sizeof(InduRTDBHeader));
    }

    std::unique_ptr<osal::ITime> time_;
    alignas(128) unsigned char buf_[sizeof(InduRTDBHeader) +
                                    kMaxPoints * sizeof(PointData)];
    PointManager*               pm_;
};

}  // namespace

TEST_F(PointManagerTimeoutTest, WriteSetsQualityGood) {
    ASSERT_TRUE(pm_->write(0, 23.5));
    EXPECT_EQ(points()[0].quality, Quality::GOOD)
        << "正常写入后质量应为 GOOD";
}

TEST_F(PointManagerTimeoutTest, MarkTimeoutSetsQuality) {
    ASSERT_TRUE(pm_->write(0, 23.5));
    ASSERT_EQ(points()[0].quality, Quality::GOOD);

    EXPECT_TRUE(pm_->mark_timeout(0));
    EXPECT_EQ(points()[0].quality, Quality::TIMEOUT);
}

TEST_F(PointManagerTimeoutTest, MarkTimeoutIncrementsCounter) {
    EXPECT_EQ(header().stats.timeouts, 0ULL);

    EXPECT_TRUE(pm_->mark_timeout(0));
    EXPECT_EQ(header().stats.timeouts, 1ULL);

    EXPECT_TRUE(pm_->mark_timeout(1));
    EXPECT_EQ(header().stats.timeouts, 2ULL);
}

TEST_F(PointManagerTimeoutTest, MarkTimeoutDoesNotCountInvalidId) {
    EXPECT_FALSE(pm_->mark_timeout(kMaxPoints));
    EXPECT_FALSE(pm_->mark_timeout(kMaxPoints + 100));

    EXPECT_EQ(header().stats.timeouts, 0ULL)
        << "越界标记不得污染计数器";
}

TEST_F(PointManagerTimeoutTest, MarkTimeoutKeepsValue) {
    ASSERT_TRUE(pm_->write(2, 12.5));

    EXPECT_TRUE(pm_->mark_timeout(2));
    EXPECT_DOUBLE_EQ(points()[2].value.d, 12.5)
        << "标记超时只改质量，不应破坏已采集的值";
}

// 【关键回归】标记路径必须释放 seqlock
TEST_F(PointManagerTimeoutTest, SeqlockReleasedAfterMarkTimeout) {
    EXPECT_TRUE(pm_->mark_timeout(0));

    EXPECT_EQ(header().write_seq & 1ULL, 0ULL)
        << "mark_timeout 后 write_seq 必须恢复为偶数";

    EXPECT_TRUE(pm_->write(3, 1.0)) << "标记之后必须仍可正常写入";

    PointData out{};
    EXPECT_TRUE(pm_->read(3, out)) << "标记之后必须仍可正常读取";
}

// 超时点位被重新写入后质量应恢复 GOOD，计数器不回退
TEST_F(PointManagerTimeoutTest, RewriteAfterTimeoutRestoresGood) {
    ASSERT_TRUE(pm_->mark_timeout(4));
    ASSERT_EQ(points()[4].quality, Quality::TIMEOUT);

    ASSERT_TRUE(pm_->write(4, 1.0));
    EXPECT_EQ(points()[4].quality, Quality::GOOD);
    EXPECT_EQ(header().stats.timeouts, 1ULL)
        << "计数器是累计值，重新写入不应回退";
}

TEST_F(PointManagerTimeoutTest, GetTimeoutCountReflectsMarks) {
    EXPECT_EQ(pm_->get_timeout_count(), 0ULL);

    ASSERT_TRUE(pm_->mark_timeout(0));
    ASSERT_TRUE(pm_->mark_timeout(0));
    ASSERT_TRUE(pm_->mark_timeout(0));

    EXPECT_EQ(pm_->get_timeout_count(), 3ULL);
}