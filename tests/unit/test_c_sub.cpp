/**
 * @file test_c_sub.cpp
 * @brief SubscriptionManager 单元测试
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include "core/irt_shm.h"
#include "core/irt_subscription.h"
}

/* 测试回调, 记录被调用信息 */
struct CallInfo { uint32_t id; char mark[16]; };

static void test_callback(uint32_t id, const indurtdb_point_t* data,
                          void* user_data) {
    auto* ci = static_cast<CallInfo*>(user_data);
    ci->id = id;
    snprintf(ci->mark, sizeof(ci->mark), "called");
}

class CSubTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&shm_, 0, sizeof(shm_));
        ASSERT_EQ(irt_shm_init(&shm_, "test5", 16, 4), 0);
        irt_sub_init(&sub_, &shm_);
    }
    void TearDown() override {
        irt_shm_shutdown(&shm_);
    }
    irt_shm_t shm_;
    irt_sub_t sub_;
};

TEST_F(CSubTest, SubscribeAndNotify) {
    CallInfo ci = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 3, test_callback, &ci), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 1u);

    indurtdb_point_t pt;
    std::memset(&pt, 0, sizeof(pt));
    pt.value.i = 42;
    irt_sub_notify(&sub_, 3, &pt);

    EXPECT_EQ(ci.id, 3u);
    EXPECT_STREQ(ci.mark, "called");
}

TEST_F(CSubTest, UnsubscribeStopsNotify) {
    CallInfo ci = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 5, test_callback, &ci), 0);
    ASSERT_EQ(irt_sub_unsubscribe(&sub_, 5), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 0u);

    indurtdb_point_t pt{};
    irt_sub_notify(&sub_, 5, &pt);
    EXPECT_EQ(ci.id, 0u);  /* 未被调用 */
}

TEST_F(CSubTest, MultipleSubscribersSamePoint) {
    CallInfo c1 = {}, c2 = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 7, test_callback, &c1), 0);
    ASSERT_EQ(irt_sub_subscribe(&sub_, 7, test_callback, &c2), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 2u);

    indurtdb_point_t pt{};
    irt_sub_notify(&sub_, 7, &pt);
    EXPECT_STREQ(c1.mark, "called");
    EXPECT_STREQ(c2.mark, "called");
}

TEST_F(CSubTest, NotifyOnlyMatchingId) {
    CallInfo c1 = {}, c2 = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 1, test_callback, &c1), 0);
    ASSERT_EQ(irt_sub_subscribe(&sub_, 2, test_callback, &c2), 0);

    indurtdb_point_t pt{};
    irt_sub_notify(&sub_, 1, &pt);
    EXPECT_STREQ(c1.mark, "called");
    EXPECT_STREQ(c2.mark, "");  /* id=2 未被通知 */
}

TEST_F(CSubTest, UnsubscribeAllSameId) {
    /* 同一 id 多个订阅, unsubscribe 一次性全部删除 */
    CallInfo c1 = {}, c2 = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 9, test_callback, &c1), 0);
    ASSERT_EQ(irt_sub_subscribe(&sub_, 9, test_callback, &c2), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 2u);

    ASSERT_EQ(irt_sub_unsubscribe(&sub_, 9), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 0u);

    indurtdb_point_t pt{};
    irt_sub_notify(&sub_, 9, &pt);
    EXPECT_STREQ(c1.mark, "");  /* 全部已删除 */
    EXPECT_STREQ(c2.mark, "");
}

TEST_F(CSubTest, UnsubscribePreservesOtherId) {
    /* 删除 id=3 的订阅, 不影响 id=4 */
    CallInfo c1 = {}, c2 = {};
    ASSERT_EQ(irt_sub_subscribe(&sub_, 3, test_callback, &c1), 0);
    ASSERT_EQ(irt_sub_subscribe(&sub_, 4, test_callback, &c2), 0);

    ASSERT_EQ(irt_sub_unsubscribe(&sub_, 3), 0);
    EXPECT_EQ(irt_sub_count(&sub_), 1u);

    indurtdb_point_t pt{};
    irt_sub_notify(&sub_, 3, &pt);
    EXPECT_STREQ(c1.mark, "");       /* id=3 已删除 */
    irt_sub_notify(&sub_, 4, &pt);
    EXPECT_STREQ(c2.mark, "called"); /* id=4 仍有效 */
}
