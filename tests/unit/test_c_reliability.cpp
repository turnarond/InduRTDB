/**
 * @file test_c_reliability.cpp
 * @brief T7：可靠性（issue #19）—— 热写者自旋下超时扫描不再饿死 + scan_skipped 可观测
 *
 * 复现并固化 issue #19 的修复（L1 写锁冲突退避 + L2 scan_skipped 计数）：
 *  - NotStarvedByHotWriter：单个热写者持续自旋写单点时，超时扫描仍能检测到全部陈旧点
 *    （修复前全局单 seqlock 偶数窗口极短，扫描整轮取不到写锁而返回 0）。
 *  - SkippedIsObservable：极端并发下扫描会跳过部分点，必须被 scan_skipped 计数（可观测而非静默）。
 *  - NoRegressionOnIdle：无并发写时全部陈旧点被检测，且 scan_skipped 保持 0（无静默跳过）。
 *
 * 注意：indurtdb_check_timeouts 对 timeout_ns==0 直接返回 0（视为不检查），故本测试使用
 * 1ms 超时，并在 SetUp 中将陈旧点老化 >1ms，确保它们确实超时。
 */
#include <indurtdb/indurtdb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

static const uint32_t kPoints    = 64;
static const uint32_t kHotPoint  = 0;  /* 热写者自旋点：始终新鲜，永不超时 */
static const uint32_t kStaleBase = 1;  /* 点 1..63 设为陈旧（写一次后不再写） */
static const uint64_t kTimeoutNs = 1000000;  /* 1ms 超时 */

class Issue19Test : public ::testing::Test {
protected:
    void SetUp() override {
        indurtdb_shutdown();
        ASSERT_EQ(indurtdb_initialize("t7_issue19", kPoints, 4), 0);
        for (uint32_t id = kStaleBase; id < kPoints; ++id)
            ASSERT_EQ(indurtdb_write_int32(id, (int32_t)id), 0);
        /* 老化陈旧点，使其超过 1ms 超时阈值 */
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        ASSERT_EQ(indurtdb_get_scan_skipped(), 0u);
    }
    void TearDown() override { indurtdb_shutdown(); }

    /* 热写者：持续刷新 kHotPoint 的时间戳，使其永远新鲜 */
    static void spin_writer(std::atomic<bool>* stop) {
        while (!stop->load(std::memory_order_relaxed))
            indurtdb_write_int32(kHotPoint, 7);
    }
};

/* L1：单个热写者自旋下，超时扫描应能检测到全部陈旧点（修复前会整轮返回 0） */
TEST_F(Issue19Test, NotStarvedByHotWriter) {
    std::atomic<bool> stop{false};
    std::thread writer(spin_writer, &stop);

    uint64_t total = 0;
    for (int r = 0; r < 50; ++r)
        total += (uint64_t)indurtdb_check_timeouts(kTimeoutNs);

    stop.store(true, std::memory_order_relaxed);
    writer.join();

    /* 全部 63 个陈旧点都应被检测到 -> 无饿死。点 0 偶尔被计入亦不影响下限。 */
    EXPECT_GE(total, (uint64_t)(kPoints - 1));
}

/* L2：极端并发下扫描会跳过部分点（让步退避仍冲突），必须被 scan_skipped 计数 */
TEST_F(Issue19Test, SkippedIsObservable) {
    const int kWriters = 8;
    std::atomic<bool> stop{false};
    std::vector<std::thread> writers;
    for (int i = 0; i < kWriters; ++i)
        writers.emplace_back(spin_writer, &stop);

    for (int r = 0; r < 100; ++r)
        indurtdb_check_timeouts(kTimeoutNs);

    stop.store(true, std::memory_order_relaxed);
    for (auto& w : writers) w.join();

    /* 极端并发使扫描者的有限退避耗尽、跳过部分点 -> 必须被观测到（而非静默） */
    EXPECT_GT(indurtdb_get_scan_skipped(), 0u);
}

/* 回归：无并发写时，所有陈旧点被检测，且 scan_skipped 保持 0（无静默跳过） */
TEST_F(Issue19Test, NoRegressionOnIdle) {
    EXPECT_EQ((uint64_t)indurtdb_check_timeouts(kTimeoutNs), (uint64_t)(kPoints - 1));
    EXPECT_EQ(indurtdb_get_scan_skipped(), 0u);
}
