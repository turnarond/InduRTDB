/**
 * @file test_c_pm.cpp
 * @brief PointManager 单元测试
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <chrono>
#include <thread>

extern "C" {
#include "osal/irt_osal.h"
#include "core/irt_shm.h"
#include "core/irt_point_manager.h"
#include "core/irt_meta_timeout.h"
}

class CPmTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&shm_, 0, sizeof(shm_));
        ASSERT_EQ(irt_shm_init(&shm_, "test4", 32, 4), 0);
        irt_pm_init(&pm_, &shm_);
    }
    void TearDown() override {
        irt_shm_shutdown(&shm_);
    }
    irt_shm_t shm_;
    irt_pm_t  pm_;
};

TEST_F(CPmTest, WriteAndReadBool) {
    ASSERT_EQ(irt_pm_write_bool(&pm_, 0, true), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 0, &p), 0);
    EXPECT_EQ(p.value.b, true);
    EXPECT_EQ(p.type, INDURTDB_TYPE_BOOL);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
    EXPECT_GT(p.timestamp_ns, 0u);
    /* v3.9.1：入库时刻必须是墙钟 epoch，而非开机以来的单调时钟。
     * 若源头退回 irt_time_now_ns()，与 REALTIME 的差值会达到数年。 */
    uint64_t wall = irt_time_realtime_ns();
    uint64_t diff = p.timestamp_ns > wall ? p.timestamp_ns - wall : wall - p.timestamp_ns;
    EXPECT_LT(diff, 86400ULL * 1000000000ULL)
        << "point timestamp is not wall clock: ts=" << p.timestamp_ns
        << " wall=" << wall;
}

TEST_F(CPmTest, WriteAndReadInt32) {
    ASSERT_EQ(irt_pm_write_int32(&pm_, 1, -42), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 1, &p), 0);
    EXPECT_EQ(p.value.i, -42);
    EXPECT_EQ(p.type, INDURTDB_TYPE_INT32);
}

TEST_F(CPmTest, WriteAndReadDouble) {
    ASSERT_EQ(irt_pm_write_double(&pm_, 2, 3.14), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 2, &p), 0);
    EXPECT_DOUBLE_EQ(p.value.d, 3.14);
    EXPECT_EQ(p.type, INDURTDB_TYPE_DOUBLE);
}

TEST_F(CPmTest, WriteAndReadString) {
    ASSERT_EQ(irt_pm_write_string(&pm_, 3, "hello"), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 3, &p), 0);
    EXPECT_STREQ(p.value.str, "hello");
    EXPECT_EQ(p.type, INDURTDB_TYPE_STRING);
}

TEST_F(CPmTest, PeekZeroCopy) {
    ASSERT_EQ(irt_pm_write_int32(&pm_, 5, 99), 0);
    const indurtdb_point_t* pp = irt_pm_peek(&pm_, 5);
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->value.i, 99);
    /* peek 返回的是共享内存指针, 不是副本 */
}

TEST_F(CPmTest, InvalidIdRejected) {
    EXPECT_FALSE(irt_pm_validate_id(&pm_, 32));  /* max_points=32, id 32 越界 */
    EXPECT_EQ(irt_pm_write_int32(&pm_, 32, 1), -1);
    indurtdb_point_t p;
    EXPECT_EQ(irt_pm_read(&pm_, 32, &p), -1);
    EXPECT_EQ(irt_pm_peek(&pm_, 32), nullptr);
}

TEST_F(CPmTest, WriteCountIncrements) {
    uint64_t c0 = irt_pm_write_count(&pm_);
    irt_pm_write_bool(&pm_, 10, true);
    irt_pm_write_int32(&pm_, 11, 1);
    uint64_t c1 = irt_pm_write_count(&pm_);
    EXPECT_EQ(c1, c0 + 2);
}

/* ============================================================
 * v3.10: per-point timeout check_timeouts 语义
 * ============================================================ */

namespace {

std::string unique_instance(const char* tag) {
    static int seq = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "irt_pm_%s_%d_%d", tag, (int)getpid(), ++seq);
    return std::string(buf);
}

void write_point_and_meta(irt_shm_t* shm, irt_pm_t* pm, uint32_t id, uint32_t timeout_ms) {
    ASSERT_EQ(irt_pm_write_double(pm, id, 1.0), 0);
    if (timeout_ms > 0) {
        indurtdb_meta_t m{};
        irt_meta_set_timeout_ms(&m, timeout_ms);
        ASSERT_EQ(irt_meta_set(shm, id, &m), 0);
    }
}

} // namespace

/* 全局 1000 ms，点位 10 ms；写后睡 50 ms → check_timeouts(0) 命中 */
TEST_F(CPmTest, CheckTimeoutsUsesPerPoint)
{
    write_point_and_meta(&shm_, &pm_, 10, 10);  // per-point 10 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int detected = irt_pm_check_timeouts(&pm_, 1000 * 1000000ULL);  // global 1000 ms
    EXPECT_GT(detected, 0);

    indurtdb_point_t p{};
    ASSERT_EQ(irt_pm_read(&pm_, 10, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_TIMEOUT);
}

/* 全局 0，点位 10 ms；写后睡 50 ms → check_timeouts(0) 仍命中 */
TEST_F(CPmTest, CheckTimeoutsZeroStillScans)
{
    write_point_and_meta(&shm_, &pm_, 11, 10);  // per-point 10 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int detected = irt_pm_check_timeouts(&pm_, 0);  // global 0
    EXPECT_GT(detected, 0);

    indurtdb_point_t p{};
    ASSERT_EQ(irt_pm_read(&pm_, 11, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_TIMEOUT);
}

/* 点位 500 ms，全局 10 ms；写后睡 50 ms → 不命中（per-point 优先） */
TEST_F(CPmTest, CheckTimeoutsPerPointOverridesGlobal)
{
    write_point_and_meta(&shm_, &pm_, 12, 500);  // per-point 500 ms
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int detected = irt_pm_check_timeouts(&pm_, 10 * 1000000ULL);  // global 10 ms
    EXPECT_EQ(detected, 0);

    indurtdb_point_t p{};
    ASSERT_EQ(irt_pm_read(&pm_, 12, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_GOOD);
}

/* 未配置点位，全局 10 ms；写后睡 50 ms → 命中 */
TEST_F(CPmTest, CheckTimeoutsGlobalStillWorks)
{
    write_point_and_meta(&shm_, &pm_, 13, 0);  // no per-point
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    int detected = irt_pm_check_timeouts(&pm_, 10 * 1000000ULL);  // global 10 ms
    EXPECT_GT(detected, 0);

    indurtdb_point_t p{};
    ASSERT_EQ(irt_pm_read(&pm_, 13, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_TIMEOUT);
}

TEST_F(CPmTest, CheckTimeoutsPreservesLimitAndDoesNotRepeat)
{
    write_point_and_meta(&shm_, &pm_, 14, 10);
    ASSERT_EQ(irt_pm_set_quality(
                  &pm_, 14,
                  INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_GOOD, INDURTDB_LIMIT_HIGH)),
              0);
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    EXPECT_EQ(irt_pm_check_timeouts(&pm_, 0), 1);

    indurtdb_point_t p{};
    ASSERT_EQ(irt_pm_read(&pm_, 14, &p), 0);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(p.quality), INDURTDB_QUALITY_TIMEOUT);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(p.quality), INDURTDB_LIMIT_HIGH);

    EXPECT_EQ(irt_pm_check_timeouts(&pm_, 0), 0);
}
