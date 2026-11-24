/**
 * @file test_c_epoch.cpp
 * @brief S1 全局 epoch 原语单测：单调非零、陈旧帧拒绝、advance_to、字典序键。
 */
#include <gtest/gtest.h>

extern "C" {
#include "cluster/rtdbd_epoch.h"
}

#include <cstring>
#include <string>

TEST(RtdbdEpoch, NextMonotonicAndNonZero)
{
    rtdbd_epoch_reset_for_test();
    uint64_t a = rtdbd_epoch_next();
    uint64_t b = rtdbd_epoch_next();
    uint64_t c = rtdbd_epoch_next();
    EXPECT_NE(a, 0u);
    EXPECT_GT(b, a);
    EXPECT_GT(c, b);
    EXPECT_EQ(rtdbd_epoch_current(), c);
}

TEST(RtdbdEpoch, StaleFrameRejected)
{
    rtdbd_epoch_reset_for_test();
    uint64_t e1 = rtdbd_epoch_next();
    uint64_t e2 = rtdbd_epoch_next();
    EXPECT_FALSE(rtdbd_epoch_is_stale(e2));   // 当前帧
    EXPECT_TRUE(rtdbd_epoch_is_stale(e1));    // 旧帧被拒（epoch < current）
    EXPECT_FALSE(rtdbd_epoch_is_stale(e2 + 1)); // 未来帧由 S2 backlog 处理，不在此判 stale
}

TEST(RtdbdEpoch, AdvanceTo)
{
    rtdbd_epoch_reset_for_test();
    (void)rtdbd_epoch_next();
    rtdbd_epoch_advance_to(1000u);
    EXPECT_EQ(rtdbd_epoch_current(), 1000u);
    uint64_t n = rtdbd_epoch_next();
    EXPECT_GT(n, 1000u);
    // 小值不回退
    rtdbd_epoch_advance_to(5u);
    EXPECT_EQ(rtdbd_epoch_current(), n);
}

TEST(RtdbdEpoch, OrderingKey)
{
    rtdbd_epoch_key_t k1, k2, k3;
    ASSERT_EQ(rtdbd_epoch_key(10u, 100u, "node-a", &k1), RTDBD_EPOCH_KEY_OK);
    ASSERT_EQ(rtdbd_epoch_key(10u, 100u, "node-b", &k2), RTDBD_EPOCH_KEY_OK);
    ASSERT_EQ(rtdbd_epoch_key(11u, 100u, "node-a", &k3), RTDBD_EPOCH_KEY_OK);
    // 同 epoch 不同 node_id 可区分（字典序）
    EXPECT_NE(std::memcmp(&k1, &k2, sizeof(k1)), 0);
    // epoch 为主序：epoch 11 > 10
    EXPECT_GT(rtdbd_epoch_key_compare(&k3, &k1), 0);
    EXPECT_LT(rtdbd_epoch_key_compare(&k1, &k3), 0);
}
