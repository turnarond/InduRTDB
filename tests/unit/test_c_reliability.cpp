/**
 * @file test_c_reliability.cpp
 * @brief T7：可靠性（issue #19）—— 热写者自旋下超时扫描不再饿死 + scan_skipped 可观测
 *
 * 复现并固化 issue #19 的修复（L1 写锁冲突退避 + L2 scan_skipped 计数）：
 *  - NotStarvedByHotWriter：单个热写者持续自旋写单点时，超时扫描仍能检测到全部陈旧点
 *    （修复前全局单 seqlock 偶数窗口极短，扫描整轮取不到写锁而返回 0）。
 *  - ScanSkippedContract.SafeBeforeInit / Issue19Test.StaysZeroWithoutContention /
 *    MonotonicAcrossScans：以**确定性**断言覆盖 scan_skipped 的可观测性契约。
 *  - NoRegressionOnIdle：无并发写时全部陈旧点被检测，且 scan_skipped 保持 0（无静默跳过）。
 *
 * 关于「并发下必然发生跳过」的断言：见本文件末尾 SkippedIsObservable 一节的
 * 移除说明 —— 该性质无法通过公开 API 确定性构造，只能依赖线程调度概率，
 * 实测在 2~4 核机器上失败率 40~50%，作为门禁用例会长期误报。
 *
 * 注意：indurtdb_check_timeouts 对 timeout_ns==0 直接返回 0（视为不检查），故本测试使用
 * 1ms 超时，并在 SetUp 中将陈旧点老化 >1ms，确保它们确实超时。
 */
#include <indurtdb/indurtdb.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <thread>

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

/* L2 回归：未初始化时不得崩溃，getter 必须安全返回 0。
 * 用独立 TEST（非 TEST_F）以便先 shutdown 再调用。 */
TEST(ScanSkippedContract, SafeBeforeInit) {
    indurtdb_shutdown();
    EXPECT_EQ(indurtdb_get_scan_skipped(), 0u);
}

/* L2 回归：无并发时 scan_skipped 必须恒为 0（多轮扫描后仍不得出现静默跳过）。
 * 这是 scan_skipped 唯一可确定性验证的不变量：跳过只在写锁冲突时计数，
 * 无写者即无冲突，故必须严格为 0。 */
TEST_F(Issue19Test, StaysZeroWithoutContention) {
    for (int r = 0; r < 200; ++r)
        indurtdb_check_timeouts(kTimeoutNs);
    EXPECT_EQ(indurtdb_get_scan_skipped(), 0u);
}

/* L2 回归：计数单调不减 —— 库内不存在把 scan_skipped 清零的路径。
 * 若有人误将其重置，该断言会失败。 */
TEST_F(Issue19Test, MonotonicAcrossScans) {
    uint64_t prev = indurtdb_get_scan_skipped();
    for (int r = 0; r < 50; ++r) {
        indurtdb_check_timeouts(kTimeoutNs);
        uint64_t now = indurtdb_get_scan_skipped();
        EXPECT_GE(now, prev);
        prev = now;
    }
}

/*
 * 已移除（原 Issue19Test.SkippedIsObservable）
 * ------------------------------------------------------------------
 * 原断言：8 个并发写者自旋 + 100 轮扫描后，EXPECT_GT(scan_skipped, 0)。
 *
 * 移除原因：该断言把线程调度概率当成硬契约。scan_skipped 仅在
 * irt_pm_check_timeouts 的 irt_seqlock_write_begin() 返回奇数时 +1，
 * 而 write_seq 是整段共用的**单锁**。核数少时写者自身被反复抢占，
 * 反而使全局锁空闲比例上升、扫描者更易 CAS 成功 —— 跳过次数趋于 0。
 *
 * 实测（Debug，同一 commit b940a24）：
 *     112 核       25/25 通过
 *     taskset 4 核   2/10 失败
 *     taskset 2 核   6/15 失败
 *     GitHub runner(2~4 核) 失败：actual: 0 vs 0
 * 曾致 v3.8.0 在 main 上 CI 红灯（Debug job），而同一 commit 在
 * release/3.8 上全绿 —— 纯调度差异。
 *
 * 已排除的三条补救路径（均实测证伪，不要重复尝试）：
 *  1) 加大争用强度：2 核命中率随写者数**下降**（8 写者 7/12 ->
 *     128 写者 3/12）。写者越多超订越重，锁空闲比例反而升高。
 *  2) 每轮重新老化陈旧点以放大采样：第 1 轮扫完 63 点即被标记 TIMEOUT，
 *     后续轮次无事可做（扫描器遇 TIMEOUT 直接 continue）；实测 0/12。
 *  3) 测试侧持锁线程注入 CAS 冲突：check_timeouts 的**读端自旋**
 *     （s0 & 1 则 continue）会在锁被占时等待释放后才执行 CAS，
 *     面对的是刚释放的偶数窗口，几乎必然成功。实测 delta=0/10。
 *  根因是读端自旋与写端 CAS 之间的时序不对称，非争用强度问题。
 *
 * 保留价值由上方确定性用例承担。诚实说明其局限：
 * 「删除 irt_pm_check_timeouts 中的 __atomic_fetch_add(&hdr->scan_skipped)
 *  这一行」以及「getter 退化为 return 0」两类退化，本文件均**不再覆盖**
 * （已用变异测试确认 5/5 仍通过）。要覆盖需能确定性制造 CAS 冲突的
 * 测试钩子（test-only 编译期开关），涉及产品代码，另行立项。
 */

/* 回归：无并发写时，所有陈旧点被检测，且 scan_skipped 保持 0（无静默跳过） */
TEST_F(Issue19Test, NoRegressionOnIdle) {
    EXPECT_EQ((uint64_t)indurtdb_check_timeouts(kTimeoutNs), (uint64_t)(kPoints - 1));
    EXPECT_EQ(indurtdb_get_scan_skipped(), 0u);
}
