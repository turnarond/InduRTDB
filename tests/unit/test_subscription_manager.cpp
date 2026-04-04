/**
 * @file test_subscription_manager.cpp
 * @brief 订阅管理器单元测试
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "gtest/gtest.h"
#include "indurtdb/core/subscription_manager_interface.hpp"
#include "indurtdb/osal/factory.hpp"

using namespace indurtdb;
using namespace indurtdb::core;

/**
 * @brief 测试订阅管理器的基本功能
 */
class SubscriptionManagerTest : public ::testing::Test {
protected:
    void SetUp() override {
        time_provider_ = osal::OSALFactory::create_time();
        subscription_manager_ = create_subscription_manager(time_provider_);
        ASSERT_NE(subscription_manager_, nullptr);
    }

    void TearDown() override {
        subscription_manager_.reset();
        time_provider_.reset();
    }

    std::shared_ptr<ITime> time_provider_;
    std::unique_ptr<ISubscriptionManager> subscription_manager_;
};

/**
 * @brief 测试空指针时间提供者的构造
 */
TEST_F(SubscriptionManagerTest, ConstructorWithNullTimeProvider) {
    auto manager = create_subscription_manager(nullptr);
    ASSERT_NE(manager, nullptr);
}

/**
 * @brief 测试基本订阅功能
 */
TEST_F(SubscriptionManagerTest, BasicSubscribe) {
    PointId id = 100;
    bool callback_called = false;

    auto callback = [&callback_called](const PointData& data) {
        callback_called = true;
    };

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    EXPECT_EQ(subscription_manager_->get_subscription_count(), 1);
    EXPECT_EQ(subscription_manager_->get_subscription_count(id), 1);
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id));
}

/**
 * @brief 测试重复订阅
 */
TEST_F(SubscriptionManagerTest, DuplicateSubscribe) {
    PointId id = 100;
    int call_count = 0;

    auto callback = [&call_count](const PointData& data) {
        call_count++;
    };

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    EXPECT_FALSE(subscription_manager_->subscribe(id, callback));
}

/**
 * @brief 测试空回调订阅
 */
TEST_F(SubscriptionManagerTest, NullCallbackSubscribe) {
    PointId id = 100;
    EXPECT_FALSE(subscription_manager_->subscribe(id, nullptr));
}

/**
 * @brief 测试取消订阅
 */
TEST_F(SubscriptionManagerTest, Unsubscribe) {
    PointId id = 100;

    auto callback = [](const PointData& data) {};

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    EXPECT_TRUE(subscription_manager_->unsubscribe(id));
    EXPECT_EQ(subscription_manager_->get_subscription_count(), 0);
    EXPECT_FALSE(subscription_manager_->has_subscriptions(id));
}

/**
 * @brief 测试通知功能
 */
TEST_F(SubscriptionManagerTest, Notify) {
    PointId id = 100;
    bool callback_called = false;
    PointData received_data;

    auto callback = [&callback_called, &received_data](const PointData& data) {
        callback_called = true;
        received_data = data;
    };

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));

    PointData test_data;
    test_data.timestamp_ns = time_provider_->now_ns();
    test_data.type = PointType::INT32;
    test_data.value.i = 12345;

    EXPECT_TRUE(subscription_manager_->notify(id, test_data));
    EXPECT_TRUE(callback_called);
    EXPECT_EQ(received_data.timestamp_ns, test_data.timestamp_ns);
    EXPECT_EQ(received_data.type, test_data.type);
    EXPECT_EQ(received_data.value.i, test_data.value.i);
}

/**
 * @brief 测试心跳更新
 */
TEST_F(SubscriptionManagerTest, UpdateHeartbeat) {
    PointId id = 100;

    auto callback = [](const PointData& data) {};

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    EXPECT_TRUE(subscription_manager_->update_heartbeat(id));
}

/**
 * @brief 测试超时清理
 */
TEST_F(SubscriptionManagerTest, CleanupTimeoutSubscriptions) {
    PointId id = 100;

    auto callback = [](const PointData& data) {};

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    
    // 清理没有超时的订阅
    EXPECT_EQ(subscription_manager_->cleanup_timeout_subscriptions(1000000000), 0);
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id));
}

/**
 * @brief 测试清空所有订阅
 */
TEST_F(SubscriptionManagerTest, ClearAllSubscriptions) {
    PointId id1 = 100;
    PointId id2 = 200;

    auto callback = [](const PointData& data) {};

    EXPECT_TRUE(subscription_manager_->subscribe(id1, callback));
    EXPECT_TRUE(subscription_manager_->subscribe(id2, callback));
    EXPECT_EQ(subscription_manager_->get_subscription_count(), 2);

    EXPECT_TRUE(subscription_manager_->clear_all_subscriptions());
    EXPECT_EQ(subscription_manager_->get_subscription_count(), 0);
    EXPECT_FALSE(subscription_manager_->has_subscriptions(id1));
    EXPECT_FALSE(subscription_manager_->has_subscriptions(id2));
}

/**
 * @brief 测试状态验证
 */
TEST_F(SubscriptionManagerTest, Validate) {
    EXPECT_TRUE(subscription_manager_->validate());

    PointId id = 100;
    auto callback = [](const PointData& data) {};
    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));
    EXPECT_TRUE(subscription_manager_->validate());
}

/**
 * @brief 测试多个点位的订阅管理
 */
TEST_F(SubscriptionManagerTest, MultiplePoints) {
    PointId id1 = 100;
    PointId id2 = 200;
    PointId id3 = 300;

    auto callback = [](const PointData& data) {};

    EXPECT_TRUE(subscription_manager_->subscribe(id1, callback));
    EXPECT_TRUE(subscription_manager_->subscribe(id2, callback));
    EXPECT_TRUE(subscription_manager_->subscribe(id3, callback));

    EXPECT_EQ(subscription_manager_->get_subscription_count(), 3);
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id1));
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id2));
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id3));

    EXPECT_TRUE(subscription_manager_->unsubscribe(id2));
    EXPECT_EQ(subscription_manager_->get_subscription_count(), 2);
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id1));
    EXPECT_FALSE(subscription_manager_->has_subscriptions(id2));
    EXPECT_TRUE(subscription_manager_->has_subscriptions(id3));
}

/**
 * @brief 测试通知时的异常处理
 */
TEST_F(SubscriptionManagerTest, NotifyWithException) {
    PointId id = 100;

    auto callback = [](const PointData& data) {
        throw std::runtime_error("Test exception");
    };

    EXPECT_TRUE(subscription_manager_->subscribe(id, callback));

    PointData test_data;
    test_data.timestamp_ns = time_provider_->now_ns();
    test_data.type = PointType::BOOL;
    test_data.value.b = true;

    EXPECT_TRUE(subscription_manager_->notify(id, test_data));
}

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
