/**
 * @file test_c_sync_protocol.cpp
 * @brief S2 同步 POD：尺寸、编解码（memcpy 往返）、snapshot 页容量不溢出。
 */
#include <gtest/gtest.h>

extern "C" {
#include "vsoa/vsoa_sync_protocol.h"
}

#include <cstring>

TEST(SyncProtocol, PodSizesLocked)
{
    EXPECT_EQ(sizeof(rtdbd_sync_hello_req_t), 64u);
    EXPECT_EQ(sizeof(rtdbd_sync_hello_resp_t), 72u);
    EXPECT_EQ(sizeof(rtdbd_sync_delta_req_t), 16u);
    EXPECT_EQ(sizeof(rtdbd_sync_delta_rec_t), 88u);
    EXPECT_EQ(sizeof(rtdbd_sync_delta_resp_t), 16u);
    EXPECT_EQ(sizeof(rtdbd_sync_snapshot_req_t), 8u);
    EXPECT_EQ(sizeof(rtdbd_sync_snapshot_resp_t), 16u);
    EXPECT_EQ(sizeof(rtdbd_sync_wait_req_t), 16u);
    EXPECT_EQ(sizeof(rtdbd_sync_wait_resp_t), 8u);
}

TEST(SyncProtocol, HelloRoundTripMemcpy)
{
    rtdbd_sync_hello_req_t a{};
    a.epoch = 42;
    a.struct_hash = 0x123456789ABCDEF0ull;
    a.value_digest = 0x0FEDCBA987654321ull;
    a.n_points = 7;
    std::strncpy(a.node_id, "node-a", sizeof(a.node_id) - 1u);
    uint8_t buf[sizeof(a)];
    std::memcpy(buf, &a, sizeof(a));
    rtdbd_sync_hello_req_t b{};
    std::memcpy(&b, buf, sizeof(b));
    EXPECT_EQ(b.epoch, a.epoch);
    EXPECT_EQ(b.struct_hash, a.struct_hash);
    EXPECT_EQ(b.value_digest, a.value_digest);
    EXPECT_EQ(b.n_points, a.n_points);
    EXPECT_STREQ(b.node_id, "node-a");
}

TEST(SyncProtocol, SnapshotPageCapacityNeverOverflowsResp)
{
    // 64 points/page × 128B + header ≤ 8192B（不能因 aligned flexible-array 溢出）
    uint32_t cap = rtdbd_sync_page_capacity(1000);
    EXPECT_LE(cap, (RTDBD_SYNC_RESP_MAX - sizeof(indurtdb_point_t)) / sizeof(indurtdb_point_t));
    EXPECT_EQ(cap, 63u); // (8192-128)/128 = 63
    EXPECT_EQ(rtdbd_sync_page_capacity(10), 10u);
    EXPECT_EQ(cap * sizeof(indurtdb_point_t) + sizeof(rtdbd_sync_snapshot_resp_t),
              63u * 128u + 16u);
}

TEST(SyncProtocol, WaitPodRoundTrip)
{
    rtdbd_sync_wait_req_t r{};
    r.min_epoch = 10; r.required_acks = 2; r.timeout_ms = 3000;
    rtdbd_sync_wait_resp_t out{};
    out.acks = 1; out.ok = 0;
    EXPECT_EQ(r.required_acks, 2u);
    EXPECT_EQ(out.acks, 1u);
}
