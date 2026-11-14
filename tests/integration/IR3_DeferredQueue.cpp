/**
 * @file IR3_DeferredQueue.cpp
 * @brief v3.12 M3.2 ③：延迟动作队列接入主循环的执行器语义（§8.3-3）。
 *
 * 用例名固定 IR3_DeferredQueue（§11.1 required_irs，不得改名）。
 * 覆盖：回调内不执行（callback_reconnect_attempts=0 口径）、每轮 32 预算、
 * 「目标已失效 → no-op」（A2 generation）、执行器缺失不假装执行、
 * PEER_ADD/PEER_DEL 正确分派、FIFO 顺序。
 *
 * 纯逻辑用例：默认构建（INDURTDB_ENABLE_VSOA=OFF）即可运行，不依赖 SDK。
 */
#include <gtest/gtest.h>

#include "cluster/rtdbd_defer_actions.h"

#include <cstring>
#include <vector>

namespace {

struct Spy {
    uint32_t generation = 7u;
    bool     alive = true;
    int reconnect = 0, close = 0, rebuild = 0;
    int fullsync = 0, peer_add = 0, peer_del = 0;
    std::vector<uint32_t> order;
    uint64_t executed = 0, stale = 0, unavailable = 0;
};

uint32_t spy_generation(void *o, uint32_t) { return static_cast<Spy *>(o)->generation; }
bool spy_alive(void *o, uint32_t) { return static_cast<Spy *>(o)->alive; }

void on_reconnect(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->reconnect++; s->order.push_back(sid);
}
void on_close(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->close++; s->order.push_back(sid);
}
void on_rebuild(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->rebuild++; s->order.push_back(sid);
}
void on_fullsync(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->fullsync++; s->order.push_back(sid);
}
void on_peer_add(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->peer_add++; s->order.push_back(sid);
}
void on_peer_del(void *o, uint32_t sid, uint32_t) {
    Spy *s = static_cast<Spy *>(o); s->peer_del++; s->order.push_back(sid);
}

rtdbd_defer_exec_t make_exec(Spy *s, bool with_all = false)
{
    rtdbd_defer_exec_t e;
    std::memset(&e, 0, sizeof(e));
    e.session_generation = spy_generation;
    e.session_alive = spy_alive;
    e.on_close = on_close;
    e.on_rebuild_subs = on_rebuild;
    e.on_peer_add = on_peer_add;
    e.on_peer_del = on_peer_del;
    /* 默认故意缺 RECONNECT / TRIGGER_FULLSYNC 执行器 → 计 unavailable */
    if (with_all) {
        e.on_reconnect = on_reconnect;
        e.on_trigger_fullsync = on_fullsync;
    }
    e.owner = s;
    e.executed = &s->executed;
    e.stale = &s->stale;
    e.unavailable = &s->unavailable;
    return e;
}

} // namespace

TEST(IR3_DeferredQueue, EnqueueDoesNotExecuteUntilDrain)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s, true);

    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_RECONNECT, 3, 0, 7u));
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_REBUILD_SUBS, 4, 0, 7u));
    ASSERT_EQ(rtdbd_defer_count(&q), 2u);
    /* §11.1 P6：callback_reconnect_attempts 须为 0 —— 入队绝不触发回调 */
    EXPECT_EQ(s.reconnect, 0);
    EXPECT_EQ(s.rebuild, 0);
    EXPECT_EQ(s.executed, 0u);

    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 2u);
    EXPECT_EQ(s.reconnect, 1);
    EXPECT_EQ(s.rebuild, 1);
    EXPECT_EQ(s.executed, 2u);
    EXPECT_EQ(rtdbd_defer_count(&q), 0u);
}

TEST(IR3_DeferredQueue, DrainRespectsPerRoundBudget)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s);
    /* 集群级动作不校验 generation，便于纯粹验证预算 */
    for (uint32_t i = 0; i < 100u; ++i) {
        ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_PEER_ADD, i, 0, 0u));
    }
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, RTDBD_DEFER_DRAIN_BUDGET, &e), 32u);
    EXPECT_EQ(rtdbd_defer_count(&q), 68u);
    EXPECT_EQ(s.peer_add, 32);
    /* 再跑两轮把剩余排空 */
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, RTDBD_DEFER_DRAIN_BUDGET, &e), 32u);
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, RTDBD_DEFER_DRAIN_BUDGET, &e), 32u);
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, RTDBD_DEFER_DRAIN_BUDGET, &e), 4u);
    EXPECT_EQ(rtdbd_defer_count(&q), 0u);
}

TEST(IR3_DeferredQueue, StaleGenerationIsNoOp)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s, true);
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_CLOSE, 5, 0, 7u));
    /* 模拟：入队后连接关闭、槽位被新连接复用 → generation 从 7 变 8 */
    s.generation = 8u;
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 0u);
    EXPECT_EQ(s.close, 0);
    EXPECT_EQ(s.stale, 1u);
    EXPECT_EQ(s.executed, 0u);
}

TEST(IR3_DeferredQueue, DeadTargetIsNoOp)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s);
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_REBUILD_SUBS, 1, 0, 7u));
    s.alive = false;
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 0u);
    EXPECT_EQ(s.rebuild, 0);
    EXPECT_EQ(s.stale, 1u);
}

TEST(IR3_DeferredQueue, LegacyPushWithoutGenerationIsNotStale)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s);
    /* 旧 API（generation=0 = 不校验）必须仍可执行：兼容既有入队点 */
    ASSERT_TRUE(rtdbd_defer_push(&q, RTDBD_DEFER_CLOSE, 2, 0));
    s.generation = 99u;
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 1u);
    EXPECT_EQ(s.close, 1);
    EXPECT_EQ(s.stale, 0u);
}

TEST(IR3_DeferredQueue, MissingHandlerCountedNeverFaked)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s); /* RECONNECT / FULLSYNC 执行器为 NULL */
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_RECONNECT, 1, 0, 7u));
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_TRIGGER_FULLSYNC, 0, 0, 0u));
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 0u);
    EXPECT_EQ(s.unavailable, 2u);
    EXPECT_EQ(s.executed, 0u);
    EXPECT_EQ(s.reconnect, 0);
}

TEST(IR3_DeferredQueue, PeerActionsAndFifoArePreserved)
{
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s);
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_PEER_ADD, 3, 0, 0u));
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_PEER_DEL, 1, 0, 0u));
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_REBUILD_SUBS, 2, 0, 7u));
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 3u);
    EXPECT_EQ(s.peer_add, 1);
    EXPECT_EQ(s.peer_del, 1);
    EXPECT_EQ(s.rebuild, 1);
    ASSERT_EQ(s.order.size(), 3u);
    EXPECT_EQ(s.order[0], 3u);
    EXPECT_EQ(s.order[1], 1u);
    EXPECT_EQ(s.order[2], 2u);
}

TEST(IR3_DeferredQueue, ReconnectIsSessionScopedAndDispatched)
{
    /* §8.3-3 RECONNECT 是会话级动作（targets_session 包含它），须受 generation
     * 约束；且 drain 须把它分派到 on_reconnect 执行器（切片2 接线后）。 */
    rtdbd_defer_queue_t q;
    rtdbd_defer_queue_init(&q);
    Spy s;
    rtdbd_defer_exec_t e = make_exec(&s, true); /* 含 on_reconnect spy */
    ASSERT_TRUE(rtdbd_defer_push_gen(&q, RTDBD_DEFER_RECONNECT, 5, 0, 7u));
    EXPECT_EQ(rtdbd_defer_actions_drain(&q, 32u, &e), 1u);
    EXPECT_EQ(s.reconnect, 1);
    EXPECT_EQ(s.executed, 1u);

    /* generation 失配（会话已失效/槽位复用）→ no-op（stale），绝不执行 */
    Spy s2;
    rtdbd_defer_queue_t q2;
    rtdbd_defer_queue_init(&q2);
    rtdbd_defer_exec_t e2 = make_exec(&s2, true);
    ASSERT_TRUE(rtdbd_defer_push_gen(&q2, RTDBD_DEFER_RECONNECT, 5, 0, 7u));
    s2.generation = 8u;
    EXPECT_EQ(rtdbd_defer_actions_drain(&q2, 32u, &e2), 0u);
    EXPECT_EQ(s2.reconnect, 0);
    EXPECT_EQ(s2.stale, 1u);
}
