/**
 * @file test_subscription_manager.cpp
 * @brief SubscriptionManager 单元测试（C 风格回调）
 * @version 2.0.0
 */

#include <gtest/gtest.h>
#include <indurtdb/core/subscription_manager_interface.hpp>
#include <indurtdb/osal/factory.hpp>
#include <indurtdb/types/memory_layout.hpp>
#include <cstring>

using namespace indurtdb;
using namespace indurtdb::core;

// 共享状态，供回调使用
struct TestContext {
    bool  called = false;
    int   call_count = 0;
    PointId last_id = 0;
    double last_value = 0.0;
};

static void test_callback(PointId id, const PointData& data, void* user_data) {
    auto* ctx = static_cast<TestContext*>(user_data);
    ctx->called = true;
    ctx->call_count++;
    ctx->last_id = id;
    if (data.type == PointType::DOUBLE) {
        ctx->last_value = data.value.d;
    }
}

class SubscriptionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        time_ = osal::OSALFactory::create_time();
        std::memset(&shm_table_, 0, sizeof(shm_table_));
        mgr_ = new SubscriptionManager(time_.get(), shm_table_, 4);
    }

    void TearDown() override {
        delete mgr_;
        time_.reset();
    }

    std::unique_ptr<osal::ITime> time_;
    SubscriberEntry shm_table_[4];
    SubscriptionManager* mgr_;
};

TEST_F(SubscriptionManagerTest, BasicSubscribe) {
    TestContext ctx;
    EXPECT_TRUE(mgr_->subscribe(100, test_callback, &ctx));
    EXPECT_EQ(mgr_->subscription_count(), 1u);
    EXPECT_TRUE(mgr_->has_subscriptions(100));
}

TEST_F(SubscriptionManagerTest, NullCallbackRejected) {
    EXPECT_FALSE(mgr_->subscribe(100, nullptr, nullptr));
}

TEST_F(SubscriptionManagerTest, Unsubscribe) {
    TestContext ctx;
    EXPECT_TRUE(mgr_->subscribe(100, test_callback, &ctx));
    EXPECT_TRUE(mgr_->unsubscribe(100));
    EXPECT_EQ(mgr_->subscription_count(), 0u);
    EXPECT_FALSE(mgr_->has_subscriptions(100));
}

TEST_F(SubscriptionManagerTest, NotifyTriggersCallback) {
    TestContext ctx;
    EXPECT_TRUE(mgr_->subscribe(200, test_callback, &ctx));

    PointData data;
    data.value.d = 23.5;
    data.type = PointType::DOUBLE;
    data.quality = Quality::GOOD;

    mgr_->notify(200, data);
    EXPECT_TRUE(ctx.called);
    EXPECT_EQ(ctx.last_id, 200u);
    EXPECT_DOUBLE_EQ(ctx.last_value, 23.5);
}

TEST_F(SubscriptionManagerTest, NotifyMultipleCallbacks) {
    TestContext ctx1, ctx2, ctx3;
    EXPECT_TRUE(mgr_->subscribe(100, test_callback, &ctx1));
    EXPECT_TRUE(mgr_->subscribe(100, test_callback, &ctx2));
    EXPECT_TRUE(mgr_->subscribe(100, test_callback, &ctx3));
    EXPECT_EQ(mgr_->subscription_count(100), 3u);

    PointData data;
    data.value.i = 42;
    data.type = PointType::INT32;

    mgr_->notify(100, data);
    EXPECT_EQ(ctx1.call_count, 1);
    EXPECT_EQ(ctx2.call_count, 1);
    EXPECT_EQ(ctx3.call_count, 1);
}

TEST_F(SubscriptionManagerTest, HeartbeatUpdate) {
    // 心跳写入共享内存表
    mgr_->update_heartbeat(1234);
    EXPECT_EQ(shm_table_[0].pid, 1234);
    EXPECT_GT(shm_table_[0].last_heartbeat_ns, 0ULL);
}

TEST_F(SubscriptionManagerTest, ZombieCleanup) {
    // 注册一个"旧的"心跳
    shm_table_[0].pid = 5678;
    shm_table_[0].last_heartbeat_ns = 1;  // 很久以前

    size_t cleaned = mgr_->cleanup_zombies();
    EXPECT_GE(cleaned, 1u);
    EXPECT_EQ(shm_table_[0].pid, 0);  // 应被清理
}

TEST_F(SubscriptionManagerTest, ClearAll) {
    TestContext ctx;
    EXPECT_TRUE(mgr_->subscribe(1, test_callback, &ctx));
    EXPECT_TRUE(mgr_->subscribe(2, test_callback, &ctx));
    EXPECT_EQ(mgr_->subscription_count(), 2u);

    mgr_->clear_all();
    EXPECT_EQ(mgr_->subscription_count(), 0u);
}

TEST_F(SubscriptionManagerTest, MaxCallbacksEnforced) {
    TestContext ctx;
    // 填满所有槽位
    for (size_t i = 0; i < SubscriptionManager::MAX_CALLBACKS; ++i) {
        EXPECT_TRUE(mgr_->subscribe(static_cast<PointId>(i), test_callback, &ctx));
    }
    // 第 257 个注册应失败
    EXPECT_FALSE(mgr_->subscribe(9999, test_callback, &ctx));
}

TEST_F(SubscriptionManagerTest, Validate) {
    EXPECT_TRUE(mgr_->validate());
}

// main() provided by GTest::gtest_main
