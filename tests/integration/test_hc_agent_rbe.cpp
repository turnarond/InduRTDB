/**
 * @file test_hc_agent_rbe.cpp
 * @brief h-C 设备侧代理 RBE 适配纯逻辑（OFF 可跑，无 VSOA SDK 依赖）。
 * 三例：A/B 订阅同点、A 退订 B 仍订阅、B 新订阅收首值（模拟 rtdbd per-session
 * "重订阅首值必发"语义）。
 */
#include <gtest/gtest.h>

extern "C" {
#include "agent/rtdbd_device_agent.h"
}

#include <cstring>

namespace {

TEST(HcAgentRbe, SamePointTwoListenersFirstThenSecond)
{
    agent_rbe_state_t s;
    agent_rbe_init(&s);
    // A 首个订阅点 5 → 需向上游 /subscription/add
    EXPECT_TRUE(agent_rbe_subscribe(&s, 0u, 5u));
    EXPECT_TRUE(agent_rbe_listener_has(&s, 0u, 5u));
    // B 也订阅点 5 → 非首个（引用计数 2），不需要向上游重复订阅
    EXPECT_FALSE(agent_rbe_subscribe(&s, 1u, 5u));
    EXPECT_TRUE(agent_rbe_listener_has(&s, 1u, 5u));
    // A 重复订阅 → 幂等 false
    EXPECT_FALSE(agent_rbe_subscribe(&s, 0u, 5u));
    free(s.points);
}

TEST(HcAgentRbe, AUnsubscribeBStillSubscribed)
{
    agent_rbe_state_t s;
    agent_rbe_init(&s);
    EXPECT_TRUE(agent_rbe_subscribe(&s, 0u, 5u));
    EXPECT_FALSE(agent_rbe_subscribe(&s, 1u, 5u));
    // A 退订 → 非最后（B 仍订阅），不向上游退订
    EXPECT_FALSE(agent_rbe_unsubscribe(&s, 0u, 5u));
    EXPECT_FALSE(agent_rbe_listener_has(&s, 0u, 5u));
    EXPECT_TRUE(agent_rbe_listener_has(&s, 1u, 5u));
    // B 退订 → 最后一个，需向上游 /subscription/remove
    EXPECT_TRUE(agent_rbe_unsubscribe(&s, 1u, 5u));
    free(s.points);
}

TEST(HcAgentRbe, CachedFirstValueForLateSubscriber)
{
    agent_rbe_state_t s;
    agent_rbe_init(&s);
    // A 订阅点 5，上游已消耗 RBE 并推来一条 NOTIFY（缓存）
    EXPECT_TRUE(agent_rbe_subscribe(&s, 0u, 5u));
    uint8_t nt[32] = {0};
    nt[0] = 5u; nt[8] = 42u; // rtdbd_notify_t 前 4B=point_id，value_bits 偏移 8
    agent_rbe_cache(&s, 5u, nt);
    uint8_t got[32] = {0};
    ASSERT_TRUE(agent_rbe_get_cached(&s, 5u, got));
    EXPECT_EQ(std::memcmp(nt, got, sizeof(nt)), 0);

    // B 新订阅同点 → 非首个向上游订阅，但应从缓存主动推首值（不等下一次变化）
    EXPECT_FALSE(agent_rbe_subscribe(&s, 1u, 5u));
    ASSERT_TRUE(agent_rbe_get_cached(&s, 5u, got));
    EXPECT_EQ(std::memcmp(nt, got, sizeof(nt)), 0);
    free(s.points);
}

TEST(HcAgentRbe, NoCacheForUnseenPoint)
{
    agent_rbe_state_t s;
    agent_rbe_init(&s);
    uint8_t out[32] = {0};
    EXPECT_FALSE(agent_rbe_get_cached(&s, 999u, out));
    free(s.points);
}

} // namespace
