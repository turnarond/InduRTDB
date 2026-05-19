/**
 * @file test_seqlock.cpp
 * @brief Seqlock 自由函数单元测试
 * @version 2.0.0
 */

#include <gtest/gtest.h>
#include <indurtdb/core/seqlock.hpp>
#include <indurtdb/types/memory_layout.hpp>
#include <thread>
#include <atomic>
#include <cstring>

using namespace indurtdb;
using namespace indurtdb::core;

class SeqlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&header_, 0, sizeof(header_));
        std::memset(&points_, 0, sizeof(points_));
        header_.write_seq = 0;
        header_.max_points = 4;
    }

    InduRTDBHeader header_;
    PointData      points_[4];
};

TEST_F(SeqlockTest, WriteBeginEnd) {
    uint64_t s0 = seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s0 & 1ULL, 0ULL);  // 返回偶数
    EXPECT_EQ(header_.write_seq, s0 + 1);  // 已标记为写入中

    seqlock_write_end(&header_.write_seq, s0);
    EXPECT_EQ(header_.write_seq, s0 + 2);  // 恢复偶数
}

TEST_F(SeqlockTest, WriteConflictDetection) {
    // 第一次 writer 成功
    uint64_t s0 = seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s0 & 1ULL, 0ULL);

    // 第二次 writer 应检测到冲突（write_seq 为奇数）
    uint64_t s1 = seqlock_write_begin(&header_.write_seq);
    EXPECT_EQ(s1 & 1ULL, 1ULL);  // 冲突

    seqlock_write_end(&header_.write_seq, s0);
}

TEST_F(SeqlockTest, ReadConsistency) {
    // 写入测试数据
    uint64_t s0 = seqlock_write_begin(&header_.write_seq);
    points_[0].value.i = 42;
    points_[0].type = PointType::INT32;
    points_[0].quality = Quality::GOOD;
    seqlock_write_end(&header_.write_seq, s0);

    // 读取
    const PointData* p = seqlock_read(&header_.write_seq, points_, 0);
    ASSERT_NE(p, nullptr);
    EXPECT_EQ(p->value.i, 42);
    EXPECT_EQ(p->type, PointType::INT32);
}

TEST_F(SeqlockTest, ReadRetriesDuringWrite) {
    points_[0].value.i = 100;

    std::atomic<bool> read_done(false);
    std::atomic<int>  read_value(0);

    // Reader 线程
    std::thread reader([&]() {
        const PointData* p = seqlock_read(&header_.write_seq, points_, 0);
        if (p) read_value = p->value.i;
        read_done = true;
    });

    // Writer 模拟：在 reader 可能执行期间写入
    uint64_t s0 = seqlock_write_begin(&header_.write_seq);
    points_[0].value.i = 999;
    seqlock_write_end(&header_.write_seq, s0);

    reader.join();
    EXPECT_TRUE(read_done);
    // Reader 要么读到旧值 100 要么新值 999，不会读到撕裂值
    EXPECT_TRUE(read_value == 100 || read_value == 999);
}

TEST_F(SeqlockTest, MultiReaderSingleWriter) {
    const int readers = 4;
    std::atomic<int> success_count(0);

    // 先写入初始数据
    uint64_t s0 = seqlock_write_begin(&header_.write_seq);
    points_[0].value.i = 0;
    seqlock_write_end(&header_.write_seq, s0);

    std::vector<std::thread> threads;
    for (int i = 0; i < readers; ++i) {
        threads.emplace_back([&]() {
            for (int j = 0; j < 1000; ++j) {
                const PointData* p = seqlock_read(&header_.write_seq, points_, 0);
                if (p && p->value.i >= 0) success_count++;
            }
        });
    }

    // Writer 线程
    for (int w = 0; w < 100; ++w) {
        uint64_t sw = seqlock_write_begin(&header_.write_seq);
        if (sw & 1ULL) continue;
        points_[0].value.i = w;
        seqlock_write_end(&header_.write_seq, sw);
    }

    for (auto& t : threads) t.join();
    EXPECT_GT(success_count, 0);
}

TEST_F(SeqlockTest, SequenceNumberWrapsCorrectly) {
    // 验证多次写入后序列号始终为偶数
    for (int i = 0; i < 100; ++i) {
        uint64_t s0 = seqlock_write_begin(&header_.write_seq);
        if (s0 & 1ULL) continue;  // skip conflicts
        points_[0].value.i = i;
        seqlock_write_end(&header_.write_seq, s0);
        EXPECT_EQ(header_.write_seq & 1ULL, 0ULL);
    }
}

TEST_F(SeqlockTest, ReadOutOfRange) {
    const PointData* p = seqlock_read(&header_.write_seq, points_, 999);
    // seqlock_read 不验证 id 边界——由 PointManager::validate_id 处理
    EXPECT_NE(p, nullptr);
}

// main() provided by GTest::gtest_main
