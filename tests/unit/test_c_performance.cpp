/**
 * @file test_c_performance.cpp
 * @brief SRS §4.1 性能基准测试: P99 写入/读取延迟
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <chrono>
#include <algorithm>
#include <vector>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* PF-01 [BENCHMARK]: 写入延迟 P99 ≤ 10μs (x86 参考, 最终在 ARM 验证) */
TEST(CPerformance, DISABLED_WriteLatencyP99) {
    ASSERT_EQ(indurtdb_initialize("pf_test", 50000, 8), 0);

    const int N = 50000;
    std::vector<double> latencies;
    latencies.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        indurtdb_write_int32(i % 50000, i);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
    }

    std::sort(latencies.begin(), latencies.end());
    double p99 = latencies[static_cast<size_t>(N * 0.99)];
    printf("P50=%.1f us, P99=%.1f us, P99.9=%.1f us\n",
           latencies[N/2],
           p99,
           latencies[static_cast<size_t>(N * 0.999)]);
    printf("Throughput: %.1f k writes/s\n",
           N / (latencies.back() * 1e-3));

    /* 期望 ≤ 10μs (x86 远超 ARM 性能, 此测试为基准参考) */
    EXPECT_LE(p99, 10.0);

    indurtdb_shutdown();
}

/* PF-02 [BENCHMARK]: 读取延迟 P99 */
TEST(CPerformance, DISABLED_ReadLatencyP99) {
    ASSERT_EQ(indurtdb_initialize("pf2_test", 50000, 8), 0);

    /* 预填充 */
    for (int i = 0; i < 50000; ++i) indurtdb_write_int32(i, i);

    const int N = 50000;
    std::vector<double> latencies;
    latencies.reserve(N);

    for (int i = 0; i < N; ++i) {
        auto start = std::chrono::high_resolution_clock::now();
        int32_t v;
        indurtdb_read_int32(i, &v);
        auto end = std::chrono::high_resolution_clock::now();
        latencies.push_back(
            std::chrono::duration<double, std::micro>(end - start).count());
    }

    std::sort(latencies.begin(), latencies.end());
    double p99 = latencies[static_cast<size_t>(N * 0.99)];
    printf("P50=%.1f us, P99=%.1f us, Max=%.1f us\n",
           latencies[N/2], p99, latencies.back());

    /* 期望 ≤ 5μs */
    EXPECT_LE(p99, 5.0);

    indurtdb_shutdown();
}
