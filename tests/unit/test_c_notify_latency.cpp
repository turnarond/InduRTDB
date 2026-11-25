/**
 * @file test_c_notify_latency.cpp
 * @brief S4 VSOA NOTIFY 延迟旁路环单测：写序号配对、一t0多t1、覆盖丢弃、假绿护栏。
 */
#include <gtest/gtest.h>

extern "C" {
#include "rtdbd_notify_latency.h"
}

#include <cstring>

TEST(RtdbdNotifyLatency, InitialZeroAndTransportTag)
{
    rtdbd_notify_latency_reset_for_test();
    rtdbd_notify_latency_stats_t st = rtdbd_notify_latency_stats();
    EXPECT_EQ(st.count, 0u);
    EXPECT_EQ(st.max_ns, 0u);
    EXPECT_EQ(st.last_ns, 0u);
    EXPECT_EQ(st.dropped_unpaired, 0u);
    EXPECT_STREQ(st.transport, "vsoa");
}

TEST(RtdbdNotifyLatency, LatestT0ForSamePoint)
{
    rtdbd_notify_latency_reset_for_test();
    rtdbd_notify_latency_t0(5);
    rtdbd_notify_latency_t0(5); // 同点第二次写：最新 t0
    uint64_t ns = rtdbd_notify_latency_t1(5);
    rtdbd_notify_latency_stats_t st = rtdbd_notify_latency_stats();
    EXPECT_EQ(st.count, 1u);
    EXPECT_GE(ns, 0u);
    EXPECT_EQ(st.max_ns, st.last_ns);
    EXPECT_EQ(st.last_write_seq, 2u); // 配到最新 write_seq
}

TEST(RtdbdNotifyLatency, OneT0ThreeT1ThreeSamples)
{
    rtdbd_notify_latency_reset_for_test();
    rtdbd_notify_latency_t0(7);
    (void)rtdbd_notify_latency_t1(7);
    (void)rtdbd_notify_latency_t1(7);
    (void)rtdbd_notify_latency_t1(7);
    rtdbd_notify_latency_stats_t st = rtdbd_notify_latency_stats();
    EXPECT_EQ(st.count, 3u); // 一 t0 三客户端 → 三样本（用户裁决）
    EXPECT_EQ(st.last_write_seq, 1u); // 同一 write_seq
}

TEST(RtdbdNotifyLatency, DifferentPointsIsolated)
{
    rtdbd_notify_latency_reset_for_test();
    rtdbd_notify_latency_t0(1);
    uint64_t miss = rtdbd_notify_latency_t1(999); // 无 t0 的点
    EXPECT_EQ(miss, 0u);
    EXPECT_EQ(rtdbd_notify_latency_stats().count, 0u);
    (void)rtdbd_notify_latency_t1(1);
    EXPECT_EQ(rtdbd_notify_latency_stats().count, 1u);
}

TEST(RtdbdNotifyLatency, CapacityOverwritesOldestAndCountsUnpaired)
{
    rtdbd_notify_latency_reset_for_test();
    for (uint32_t i = 0; i < 256; ++i) {
        rtdbd_notify_latency_t0(i); // 256 个不同 point，全未配对
    }
    rtdbd_notify_latency_t0(999); // 第 257 个：覆盖最旧（point 0，未配对）
    rtdbd_notify_latency_stats_t st = rtdbd_notify_latency_stats();
    EXPECT_EQ(st.dropped_unpaired, 1u);
    // 覆盖后最新条目 point=999 仍可配对
    (void)rtdbd_notify_latency_t1(999);
    EXPECT_EQ(rtdbd_notify_latency_stats().count, 1u);
}

TEST(RtdbdNotifyLatency, PairedEntryOverwriteNotCountedDropped)
{
    rtdbd_notify_latency_reset_for_test();
    rtdbd_notify_latency_t0(1);
    (void)rtdbd_notify_latency_t1(1); // 已配对（sample_count>0）
    for (uint32_t i = 0; i < 256; ++i) rtdbd_notify_latency_t0(i + 10);
    EXPECT_EQ(rtdbd_notify_latency_stats().dropped_unpaired, 0u);
}
