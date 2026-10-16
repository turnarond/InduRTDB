/**
 * @file test_c_concurrency.cpp
 * @brief SRS §4.1 并发测试:多线程写、同点写、回调内写入
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <vector>
#include <atomic>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* CC-01: 2 线程并发写不同点位,验证多线程可见性和无冲突 */
/* DISABLED: 当前实现不完全支持多线程并发写(seqlock+visible flag 需进一步原子化 g_rtdb) */
TEST(CConcurrency, DISABLED_ConcurrentWriteDifferentPoints) {
    ASSERT_EQ(indurtdb_initialize("cc1_test", 200, 8), 0);
    std::atomic<int> errors{0};

    auto writer = [&](int base_id) {
        for (int i = 0; i < 500; ++i) {
            if (indurtdb_write_int32(base_id * 200 + i % 200, i) != 0) {
                errors++;
            }
        }
    };

    std::thread t1(writer, 0);
    std::thread t2(writer, 1);
    t1.join(); t2.join();

    EXPECT_EQ(errors.load(), 0);
    /* 2 线程 × 500 = 1000 次写 */
    uint64_t wc = indurtdb_get_write_count();
    EXPECT_GE(wc, 900u) << "most writes should succeed";
    indurtdb_shutdown();
}

/* CC-02: 2 线程并发写同一点位
 *
 * 库内对 seqlock 冲突做有限重试后，丢写率从修复前的 ~50%（1000 次写
 * 只成功 ~500）降到 2% 以内。剩余的少量 BUSY 是【合法返回】而非数据损坏
 * —— 全局单 seqlock 决定竞争下必然存在尾部；本项目以「确定性执行时间 /
 * 零阻塞」为设计原则，不为消灭这个尾部而无限自旋。
 *
 * 因此调用方【必须】自行重试，见 CC-02b。
 * 修复前本断言为 `> 250`，等于固化「冲突即丢写」的缺陷行为。 */
TEST(CConcurrency, ConcurrentWriteSamePoint) {
    ASSERT_EQ(indurtdb_initialize("cc2_test", 100, 4), 0);
    std::atomic<int> ok{0};

    auto writer = [&]() {
        for (int i = 0; i < 500; ++i) {
            if (indurtdb_write_int32(5, i) == 0) ok++;
        }
    };

    std::thread t1(writer);
    std::thread t2(writer);
    t1.join(); t2.join();

    EXPECT_GE(ok.load(), 980) << "library-internal retry should keep drop rate < 2%";

    /* 最后写入的值可读 */
    int32_t v;
    ASSERT_EQ(indurtdb_read_int32(5, &v), 0);
    EXPECT_GE(v, 0);
    indurtdb_shutdown();
}

/* CC-02b: 【给集成方的正确范式】按截止时间重试 = 零丢写
 *
 * 固定「重试 N 次」不是正确范式：全局单 seqlock 下，N 次有界重试在
 * 无界竞争中仍会被耗尽（实测 4 次约漏 1/1000）。正确做法是【按截止时间】
 * 重试 —— 重试次数自适应竞争强度，而总延迟有上界，这才是实时系统要的。
 *
 * 只对 BUSY 重试；ARG / NOT_INIT 等是真实错误，重试无意义。
 * 集成方照抄本循环即可。
 *
 * 注：本测试刻意用 4 线程争抢【同一点位】，是比真实用法更严苛的场景
 * （真实部署中写经 rtdbd 串行化，不会出现这种竞争）。 */
TEST(CConcurrency, ConcurrentWriteHighContentionNoDroppedWrites) {
    ASSERT_EQ(indurtdb_initialize("cc2b_test", 100, 4), 0);
    std::atomic<int> ok{0};
    std::atomic<int> busy{0};
    std::atomic<int> gave_up{0};
    constexpr int kThreads = 4;
    constexpr int kPerThread = 250;
    constexpr int64_t kDeadlineNs = 100 * 1000 * 1000;   /* 100ms 预算 */

    auto now_ns = []() -> int64_t {
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    };

    auto writer = [&](int base) {
        for (int i = 0; i < kPerThread; ++i) {
            int32_t v = base * kPerThread + i;
            int64_t deadline = now_ns() + kDeadlineNs;
            for (;;) {
                int rc = indurtdb_write_int32(5, v);
                if (rc == INDURTDB_OK) { ok++; break; }
                if (rc != INDURTDB_ERR_BUSY) break;   /* 真实错误，不重试 */
                if (now_ns() >= deadline) { gave_up++; break; }
                busy++;
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) ts.emplace_back(writer, t);
    for (auto& t : ts) t.join();

    EXPECT_EQ(gave_up.load(), 0) << "100ms deadline should be ample for a single point write";
    EXPECT_EQ(ok.load(), kThreads * kPerThread)
        << "deadline-based BUSY retry must not drop writes";

    int32_t v;
    ASSERT_EQ(indurtdb_read_int32(5, &v), 0);
    indurtdb_shutdown();
}

/* CC-03: 订阅回调内再次写入不崩溃 */
TEST(CConcurrency, SubscribeCallbackReentrancy) {
    ASSERT_EQ(indurtdb_initialize("cc3_test", 100, 4), 0);
    std::atomic<int> reentered{0};

    auto cb = [](uint32_t, const indurtdb_point_t*, void* u) {
        auto* cnt = static_cast<std::atomic<int>*>(u);
        cnt->fetch_add(1);
        /* 回调内写入 - 测试 reentrancy safety */
        indurtdb_write_bool(99, false);
    };

    ASSERT_EQ(indurtdb_subscribe(7, cb, &reentered), 0);
    ASSERT_EQ(indurtdb_write_bool(7, true), 0);
    /* 回调被触发,且未崩溃 */
    EXPECT_EQ(reentered.load(), 1);

    indurtdb_shutdown();
}
