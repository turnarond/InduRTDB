/**
 * @file test_c_quality.cpp
 * @brief SRS §3.2 §4.3 数据质量测试:access 控制、timeout 检测、quality 字段
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>
#include <thread>
#include <atomic>
#include <chrono>

extern "C" {
#include <indurtdb/indurtdb.h>
}

class CQualityTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("dq_test", 200, 8), 0);

        /* 加载配置: id=70 为 READ_ONLY, id=71 为 READ_WRITE */
        char path[128];
        snprintf(path, sizeof(path), "/tmp/irt_dq_%d.yaml", getpid());
        FILE* f = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fputs(
            "points:\n"
            "  - id: 70\n"
            "    name: \"Sensor_RO\"\n"
            "    type: int32\n"
            "    access: 1\n"
            "  - id: 71\n"
            "    name: \"Actuator_RW\"\n"
            "    type: int32\n"
            "    access: 3\n", f);
        fclose(f);
        ASSERT_EQ(indurtdb_load_config(path), 0);
        remove(path);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* DQ-01: 只读点位拒绝写入 (SRS §4.3) */
TEST_F(CQualityTest, WriteToReadOnlyPointRejected) {
    /* id=70 的 access=READ_ONLY, 写入应被拒绝 */
    int ret = indurtdb_write_int32(70, 999);
    EXPECT_NE(ret, 0) << "READ_ONLY point must reject writes";
}

/* DQ-02: 读写点位接受写入 (SRS §4.3) */
TEST_F(CQualityTest, WriteToReadWritePointAccepted) {
    int ret = indurtdb_write_int32(71, 555);
    EXPECT_EQ(ret, 0) << "READ_WRITE point must accept writes";

    int32_t v;
    ASSERT_EQ(indurtdb_read_int32(71, &v), 0);
    EXPECT_EQ(v, 555);
}

/* DQ-03: 超时检测 (SRS §3.2) */
TEST_F(CQualityTest, TimeoutDetection) {
    /* 先写入数据, 初始 quality=GOOD */
    ASSERT_EQ(indurtdb_write_int32(80, 42), 0);
    {
        indurtdb_point_t p;
        ASSERT_EQ(indurtdb_read_point(80, &p), 0);
        EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
    }

    /* 零超时: 任何已写入的点都会超时 (now - timestamp > 0) */
    int detected = indurtdb_check_timeouts(1);
    EXPECT_GT(detected, 0) << "should detect at least one timeout";

    /* 超时计数器应为非零 */
    uint64_t tc = indurtdb_get_timeout_count();
    EXPECT_GT(tc, 0u) << "timeout counter must be > 0 after check_timeouts";

    /* 点的 quality 应变为 TIMEOUT */
    {
        indurtdb_point_t p;
        ASSERT_EQ(indurtdb_read_point(80, &p), 0);
        EXPECT_EQ(p.quality, INDURTDB_QUALITY_TIMEOUT);
    }

    /* 重复检测不重复计数 (已是 QUALITY_TIMEOUT 的点跳过) */
    int detected2 = indurtdb_check_timeouts(1);

    /* 写入后 quality 恢复为 GOOD */
    ASSERT_EQ(indurtdb_write_int32(80, 99), 0);
    {
        indurtdb_point_t p;
        ASSERT_EQ(indurtdb_read_point(80, &p), 0);
        EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
    }
}

/* DQ-04: 写入后 quality=GOOD */
TEST_F(CQualityTest, WriteSetsQualityGood) {
    ASSERT_EQ(indurtdb_write_double(72, 1.5), 0);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(72, &p), 0);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
}

/* DQ-05: peek 能直接访问 quality 字段 */
TEST_F(CQualityTest, PeekReturnsGoodQuality) {
    ASSERT_EQ(indurtdb_write_bool(73, true), 0);
    const indurtdb_point_t* pk = indurtdb_peek(73);
    ASSERT_NE(pk, nullptr);
    EXPECT_EQ(pk->quality, INDURTDB_QUALITY_GOOD);
    EXPECT_TRUE(pk->value.b);
}

/* DQ-06: check_timeouts 扫描期间被并发 writer 持续刷新的点位不会误标 TIMEOUT.
 * 验证 double-check (p->timestamp_ns >= now 守卫) 正确拦截 false-positive. */
TEST_F(CQualityTest, TimeoutDetectionConcurrentWriteSurvives) {
    const int N = 150;
    for (int i = 0; i < N; i++) {
        if (i == 70) continue;  /* id=70 被 setUp 设为 READ_ONLY, 跳过 */
        ASSERT_EQ(indurtdb_write_int32(i, i), 0);
    }

    /* writer 线程持续刷新目标点位, 使其 timestamp 总是 > now (now 在 check_timeouts 入口捕获) */
    std::atomic<bool> stop{false};
    std::atomic<int>  wcount{0};
    std::thread writer([&]() {
        int v = 0;
        while (!stop.load()) {
            indurtdb_write_int32(100, v++);
            wcount.fetch_add(1);
        }
    });

    /* 给 writer 足够时间产生写入 */
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    int detected = indurtdb_check_timeouts(1);
    stop.store(true);
    writer.join();

    EXPECT_GT(wcount.load(), 0) << "writer should have written at least once";

    /* 点位 100 被 writer 持续刷新, 其 timestamp >= now, double-check 应跳过 */
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(100, &p), 0);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD)
        << "concurrently-refreshed point must NOT be marked TIMEOUT";
}
