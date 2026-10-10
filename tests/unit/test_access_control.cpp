/**
 * @file test_access_control.cpp
 * @brief 访问控制单元测试 —— SRS §4.3「只读点位禁止写入」
 * @version 2.2.0
 *
 * 语义约定（2026-05 决策）：
 *   - access == 0（未调用 load_config 的点位）视为「未配置」，允许写入
 *   - 仅当 access == Access::READ_ONLY(1) 时拒绝写入
 *   - 拒写路径必须释放 seqlock，否则 write_seq 永久停在奇数，数据库死锁
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

// 与 SharedMemorySegment 一致的裸字节布局：Header 紧跟 PointData 数组，
// 点位数组由 base + sizeof(InduRTDBHeader) 定位。
// 这里刻意不用 struct —— PointData 的 128 字节对齐会让编译器在 Header
// 之后插入填充，导致与 PointManager 的偏移算法不一致。
class AccessControlTest : public ::testing::Test {
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

    InduRTDBHeader&       header() { return *reinterpret_cast<InduRTDBHeader*>(buf_); }
    PointData&            point(uint32_t i) { return points()[i]; }
    PointData* points() {
        return reinterpret_cast<PointData*>(buf_ + sizeof(InduRTDBHeader));
    }

    std::unique_ptr<osal::ITime> time_;
    alignas(128) unsigned char buf_[sizeof(InduRTDBHeader) +
                                    kMaxPoints * sizeof(PointData)];
    PointManager*               pm_;
};

}  // namespace

// 未配置点位（memset 后 access==0）应允许写入
TEST_F(AccessControlTest, UnconfiguredPointIsWritable) {
    ASSERT_EQ(static_cast<uint8_t>(points()[0].access), 0u)
        << "前置条件：未配置点位的 access 应为 0";

    EXPECT_TRUE(pm_->write(0, 23.5));
    EXPECT_DOUBLE_EQ(points()[0].value.d, 23.5);
}

// 显式 READ_ONLY 的点位必须拒绝写入，且值不得改变
TEST_F(AccessControlTest, ReadOnlyPointRejectsWrite) {
    points()[1].access = Access::READ_ONLY;
    points()[1].value.d = 1.0;

    EXPECT_FALSE(pm_->write(1, 99.0));
    EXPECT_DOUBLE_EQ(points()[1].value.d, 1.0)
        << "拒写后原值不得被修改";
}

// 显式 READ_WRITE 的点位必须允许写入
TEST_F(AccessControlTest, ReadWritePointAcceptsWrite) {
    points()[2].access = Access::READ_WRITE;

    EXPECT_TRUE(pm_->write(2, 42.0));
    EXPECT_DOUBLE_EQ(points()[2].value.d, 42.0);
}

// 【关键回归】拒写路径必须释放 seqlock
// 若忘记 seqlock_write_end，write_seq 永久停在奇数，
// 之后所有 write 与 seqlock_read 都会失败 —— 整个数据库死锁。
TEST_F(AccessControlTest, SeqlockReleasedAfterRejectedWrite) {
    points()[3].access = Access::READ_ONLY;

    EXPECT_FALSE(pm_->write(3, 1.0));

    EXPECT_EQ(header().write_seq & 1ULL, 0ULL)
        << "拒写后 write_seq 必须恢复为偶数，否则 seqlock 未释放";

    // 后续写入必须仍然可用
    points()[4].access = Access::READ_WRITE;
    EXPECT_TRUE(pm_->write(4, 7.0))
        << "拒写之后数据库必须仍可正常写入";

    // 后续读取也必须可用
    PointData out{};
    EXPECT_TRUE(pm_->read(4, out));
}

// 越界 ID 仍应被拒绝（与访问权限正交）
TEST_F(AccessControlTest, OutOfRangeIdRejected) {
    points()[0].access = Access::READ_WRITE;
    EXPECT_FALSE(pm_->write(kMaxPoints, 1.0));
    EXPECT_FALSE(pm_->write(kMaxPoints + 100, 1.0));
}

// 只读点位上的各数据类型一律拒绝
TEST_F(AccessControlTest, AllTypesRejectedOnReadOnly) {
    points()[5].access = Access::READ_ONLY;

    EXPECT_FALSE(pm_->write(5, true));
    EXPECT_FALSE(pm_->write(5, static_cast<int32_t>(1)));
    EXPECT_FALSE(pm_->write(5, 1.0));

    // 注意：这里必须用 const char* 变量，不能写字面量。
    // 字面量会推导成 const char[N]，走不进模板的 const char* 分支。
    const char* text = "text";
    EXPECT_FALSE(pm_->write(5, text));
}