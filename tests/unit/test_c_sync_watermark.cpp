/**
 * @file test_c_sync_watermark.cpp
 * @brief S2 纯逻辑：delta 游标不重不漏、WAIT N-1 ack 阈值、stale epoch。
 */
#include <gtest/gtest.h>

extern "C" {
#include "cluster/rtdbd_sync_logic.h"
#include "cluster/rtdbd_epoch.h"
}

#include <indurtdb/indurtdb.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>
#include <unistd.h>

namespace {

class DeltaScanTest : public ::testing::Test {
protected:
    indurtdb_t *h = nullptr;
    char inst[80] = {0};
    void SetUp() override {
        static int seq = 0;
        indurtdb_cfg_t cfg{}; cfg.max_points = 32; cfg.max_subscribers = 0;
        std::snprintf(inst, sizeof(inst), "sync_delta_%d_%d", (int)getpid(), ++seq);
        ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
        for (uint32_t id : {1u, 2u, 5u, 8u}) {
            char name[16];
            std::snprintf(name, sizeof(name), "p%u", id);
            ASSERT_EQ(indurtdb_h_create_point(h, id, name, INDURTDB_TYPE_INT32,
                                              INDURTDB_ACCESS_READ_WRITE), 0);
        }
    }
    void TearDown() override { if (h) indurtdb_h_close(h); }
};

} // namespace

TEST_F(DeltaScanTest, CursorNoDupNoMiss)
{
    rtdbd_sync_delta_rec_t a[4]{}, b[4]{};
    uint64_t ap1 = 0, ap2 = 0;
    bool more1 = false, more2 = false;
    uint32_t n1 = rtdbd_sync_delta_scan(h, 32, 0, 2, 7, "node-a", 99, a, 4, &ap1, &more1);
    ASSERT_EQ(n1, 2u);
    EXPECT_TRUE(more1);
    uint32_t n2 = rtdbd_sync_delta_scan(h, 32, ap1, 2, 7, "node-a", 99, b, 4, &ap2, &more2);
    ASSERT_EQ(n2, 2u);
    EXPECT_FALSE(more2);
    // 不重不漏：两轮 point_id 集合 {1,2} + {5,8}
    std::vector<uint32_t> ids;
    for (uint32_t i = 0; i < n1; ++i) ids.push_back(a[i].point_id);
    for (uint32_t i = 0; i < n2; ++i) ids.push_back(b[i].point_id);
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<uint32_t>{1, 2, 5, 8}));
    for (uint32_t i = 0; i < n1 + n2; ++i) {
        // 每条带 epoch/node_id/boot_id（13 号:246 sync_rec 字段）
        const rtdbd_sync_delta_rec_t &r = (i < n1) ? a[i] : b[i - n1];
        EXPECT_EQ(r.epoch, 7u);
        EXPECT_STREQ(r.node_id, "node-a");
        EXPECT_EQ(r.boot_id, 99u);
    }
    EXPECT_GT(ap2, ap1);
}

TEST(SyncWatermark, WaitAckThreshold)
{
    uint64_t peers[4] = {10, 11, 5, 20};
    uint32_t acks = 0;
    bool ok = false;
    rtdbd_sync_wait_eval(peers, 4, 10, 3, &acks, &ok);
    EXPECT_EQ(acks, 3u); // 10/11/20 >= 10
    EXPECT_TRUE(ok);     // 3 >= 3
    rtdbd_sync_wait_eval(peers, 4, 10, 4, &acks, &ok);
    EXPECT_EQ(acks, 3u);
    EXPECT_FALSE(ok);    // required_acks>1 且不足——超时返回实际 acks（D5 不回滚）
}

TEST(SyncWatermark, StaleEpochRejected)
{
    rtdbd_epoch_reset_for_test();
    uint64_t e1 = rtdbd_epoch_next();
    uint64_t e2 = rtdbd_epoch_next();
    EXPECT_TRUE(rtdbd_epoch_is_stale(e1));
    EXPECT_FALSE(rtdbd_epoch_is_stale(e2));
}

TEST(SyncWatermark, PageCount)
{
    EXPECT_EQ(rtdbd_sync_page_count(0, 63), 0u);
    EXPECT_EQ(rtdbd_sync_page_count(1, 63), 1u);
    EXPECT_EQ(rtdbd_sync_page_count(64, 63), 2u);
    EXPECT_EQ(rtdbd_sync_page_count(126, 63), 2u);
    EXPECT_EQ(rtdbd_sync_page_count(127, 63), 3u);
}
