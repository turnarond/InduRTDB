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
#include "cluster/rtdbd_sub_snapshot.h"
#include "session/rtdbd_session.h"

#include <cstdio>
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

// ---- h-A：订阅真实重建（s-0=必须自动重建、s-A′=α session_id）----
// 快照表纯逻辑：save/load 往返、身份纵深校验、A/B 同 service_id 隔离、
// session install_subs 恢复（sub_rebuild_ok 的纯逻辑代理）。ON 侧真实断链重连
// 由 test_vsoa_e2e 覆盖；此处只锁住传输无关快照与恢复语义。

static rtdbd_sub_snap_identity_t make_ident(uint32_t uid, const char *sid)
{
    rtdbd_sub_snap_identity_t id;
    std::memset(&id, 0, sizeof(id));
    id.uid = uid;
    id.partition = 0u;
    if (sid) std::snprintf(id.service_id, sizeof(id.service_id), "%s", sid);
    return id;
}

TEST(IR3_DeferredQueue, SubSnapshotSaveLoadRoundTrip)
{
    rtdbd_sub_snapshot_clear();
    auto ident = make_ident(100u, "plc-line-1");
    uint32_t pts[] = {5u, 9u, 42u};
    ASSERT_TRUE(rtdbd_sub_snapshot_save(1234u, &ident, pts, 3u));

    uint32_t out[8] = {0};
    uint32_t n = 0u;
    ASSERT_TRUE(rtdbd_sub_snapshot_load(1234u, &ident, out, 8u, &n));
    EXPECT_EQ(n, 3u);
    EXPECT_EQ(out[0], 5u);
    EXPECT_EQ(out[1], 9u);
    EXPECT_EQ(out[2], 42u);

    auto st = rtdbd_sub_snapshot_stats();
    EXPECT_EQ(st.saved, 1u);
    EXPECT_EQ(st.loaded, 1u);
}

TEST(IR3_DeferredQueue, SubSnapshotIdentityMismatchRejected)
{
    rtdbd_sub_snapshot_clear();
    auto ident = make_ident(100u, "plc-line-1");
    uint32_t pts[] = {5u};
    ASSERT_TRUE(rtdbd_sub_snapshot_save(2000u, &ident, pts, 1u));

    // 同 service_id 但不同 uid（模拟 A/B 串台尝试）→ 身份不符拒绝
    auto other = make_ident(200u, "plc-line-1");
    uint32_t out[4] = {0};
    uint32_t n = 0u;
    EXPECT_FALSE(rtdbd_sub_snapshot_load(2000u, &other, out, 4u, &n));
    EXPECT_EQ(n, 0u);
    EXPECT_EQ(rtdbd_sub_snapshot_stats().identity_mismatch, 1u);

    // 正确身份仍可恢复（快照未被误消费）
    EXPECT_TRUE(rtdbd_sub_snapshot_load(2000u, &ident, out, 4u, &n));
    EXPECT_EQ(n, 1u);
}

TEST(IR3_DeferredQueue, SubSnapshotRestoreIntoSession)
{
    rtdbd_sub_snapshot_clear();
    auto ident = make_ident(100u, "plc-line-1");
    uint32_t pts[] = {5u, 9u};
    ASSERT_TRUE(rtdbd_sub_snapshot_save(3000u, &ident, pts, 2u));

    rtdbd_session_t sess;
    ASSERT_TRUE(rtdbd_session_init(&sess, RTDBD_SUBTAB_MIN_CAP));
    uint32_t out[RTDBD_SUB_MAX] = {0};
    uint32_t n = 0u;
    ASSERT_TRUE(rtdbd_sub_snapshot_load(3000u, &ident, out, RTDBD_SUB_MAX, &n));
    EXPECT_TRUE(rtdbd_session_install_subs(&sess, out, n)); // sub_rebuild_ok 纯逻辑判据
    EXPECT_EQ(rtdbd_session_subs_count(&sess), 2u);
    EXPECT_TRUE(rtdbd_session_subs_contains(&sess, 5u));
    EXPECT_TRUE(rtdbd_session_subs_contains(&sess, 9u));
    rtdbd_session_free(&sess);
}

TEST(IR3_DeferredQueue, SubSnapshotLruAndSidZeroRejected)
{
    rtdbd_sub_snapshot_clear();
    auto ident = make_ident(100u, "plc-line-1");
    uint32_t pt = 1u;
    // sid=0 非法（0 是未启用哨兵）→ dropped
    EXPECT_FALSE(rtdbd_sub_snapshot_save(0u, &ident, &pt, 1u));
    EXPECT_GE(rtdbd_sub_snapshot_stats().dropped, 1u);

    // 填满 32 条 + 1 条 → LRU 淘汰最早 sid=1
    uint32_t before = rtdbd_sub_snapshot_stats().evicted;
    for (uint64_t sid = 1u; sid <= RTDBD_SUB_SNAP_MAX + 1u; ++sid) {
        ASSERT_TRUE(rtdbd_sub_snapshot_save(sid, &ident, &pt, 1u));
    }
    EXPECT_EQ(rtdbd_sub_snapshot_stats().evicted - before, 1u);
    uint32_t out[4] = {0}, n = 0u;
    EXPECT_FALSE(rtdbd_sub_snapshot_load(1u, &ident, out, 4u, &n)); // 已被淘汰
    EXPECT_TRUE(rtdbd_sub_snapshot_load(RTDBD_SUB_SNAP_MAX + 1u, &ident, out, 4u, &n));
}
