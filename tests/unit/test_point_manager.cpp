/**
 * @file test_point_manager.cpp
 * @brief PointManager 单元测试 —— 直接操作共享内存的点位读写
 * @version 2.1.0
 *
 * 使用 128 字节对齐的堆内存模拟共享内存段 (PointData aligned(128)),
 * 避免测试依赖真实 shm_open, 保证单元测试快速且无残留。
 */

#include <indurtdb/core/point_manager_interface.hpp>
#include <indurtdb/types/memory_layout.hpp>
#include "fake_time.hpp"
#include <gtest/gtest.h>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace {

using indurtdb::core::PointManager;
using indurtdb::InduRTDBHeader;
using indurtdb::PointData;
using indurtdb::PointType;
using indurtdb::Quality;
using indurtdb::test::FakeTime;

class PointManagerTest : public ::testing::Test {
protected:
    static constexpr uint32_t MAX_POINTS = 64;

    void SetUp() override {
        size_t sz = sizeof(InduRTDBHeader) + MAX_POINTS * sizeof(PointData);
        ASSERT_EQ(posix_memalign(&shm_, 128, sz), 0);
        std::memset(shm_, 0, sz);
        auto* header = static_cast<InduRTDBHeader*>(shm_);
        header->max_points = MAX_POINTS;
        header->write_seq = 0;
        pm_.reset(new PointManager(shm_, MAX_POINTS, &time_));
    }

    void TearDown() override { std::free(shm_); }

    void* shm_ = nullptr;
    FakeTime time_{1234};
    std::unique_ptr<PointManager> pm_;
};

// 写 double 后完整读回: 值/类型/质量/时间戳
TEST_F(PointManagerTest, WriteReadDoubleRoundTrip) {
    ASSERT_TRUE(pm_->write(1, 23.5));

    PointData out;
    ASSERT_TRUE(pm_->read(1, out));
    EXPECT_DOUBLE_EQ(out.value.d, 23.5);
    EXPECT_EQ(out.type, PointType::DOUBLE);
    EXPECT_EQ(out.quality, Quality::GOOD);
    EXPECT_EQ(out.timestamp_ns, 1234);
}

// 四种类型写读: bool / int32 / double / string
// 注意: 字符串必须用 const char* 变量, 字面量会推导为 char[N] 触发编译期拒绝
TEST_F(PointManagerTest, WriteAllTypes) {
    const char* dev_name = "Pump_01";
    ASSERT_TRUE(pm_->write(1, true));
    ASSERT_TRUE(pm_->write(2, (int32_t)42));
    ASSERT_TRUE(pm_->write(3, 3.14));
    ASSERT_TRUE(pm_->write(4, dev_name));

    PointData out;
    ASSERT_TRUE(pm_->read(1, out));
    EXPECT_TRUE(out.value.b);
    EXPECT_EQ(out.type, PointType::BOOL);

    ASSERT_TRUE(pm_->read(2, out));
    EXPECT_EQ(out.value.i, 42);
    EXPECT_EQ(out.type, PointType::INT32);

    ASSERT_TRUE(pm_->read(3, out));
    EXPECT_DOUBLE_EQ(out.value.d, 3.14);
    EXPECT_EQ(out.type, PointType::DOUBLE);

    ASSERT_TRUE(pm_->read(4, out));
    EXPECT_STREQ(out.value.str, "Pump_01");
    EXPECT_EQ(out.type, PointType::STRING);
}

// 越界点位写读均被拒绝
TEST_F(PointManagerTest, InvalidIdRejected) {
    EXPECT_FALSE(pm_->write(MAX_POINTS, 1.0));
    EXPECT_FALSE(pm_->write(100000, 1.0));
    PointData out;
    EXPECT_FALSE(pm_->read(MAX_POINTS, out));
    EXPECT_EQ(pm_->peek(MAX_POINTS), nullptr);
}

// peek 是零拷贝: 返回的指针必须指向共享内存点位数组本身
TEST_F(PointManagerTest, PeekZeroCopyReturnsSharedArrayPtr) {
    ASSERT_TRUE(pm_->write(5, 42.0));

    const PointData* pp = pm_->peek(5);
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp, pm_->points() + 5);          // 指针相等 = 零拷贝
    EXPECT_DOUBLE_EQ(pp->value.d, 42.0);
}

// 写计数递增
TEST_F(PointManagerTest, WriteCountIncrements) {
    ASSERT_TRUE(pm_->write(1, 1.0));
    ASSERT_TRUE(pm_->write(2, 2.0));
    ASSERT_TRUE(pm_->write(3, 3.0));
    EXPECT_EQ(pm_->get_write_count(), 3);
}

// 超长字符串被截断为 31 字符 + 终止符
TEST_F(PointManagerTest, StringTruncatedTo31Chars) {
    const char* long_str = "abcdefghijklmnopqrstuvwxyz0123456789"; // 36 字符
    ASSERT_TRUE(pm_->write(7, long_str));

    PointData out;
    ASSERT_TRUE(pm_->read(7, out));
    EXPECT_EQ(std::strlen(out.value.str), 31);
    EXPECT_EQ(std::strncmp(out.value.str, long_str, 31), 0);
}

// 写入刷新时间戳: FakeTime 前移后写入, 新时间戳生效
TEST_F(PointManagerTest, TimestampRefreshedOnWrite) {
    ASSERT_TRUE(pm_->write(1, 1.0));
    time_.set_now(9999);
    ASSERT_TRUE(pm_->write(1, 2.0));

    PointData out;
    ASSERT_TRUE(pm_->read(1, out));
    EXPECT_EQ(out.timestamp_ns, 9999);
}

} // namespace
