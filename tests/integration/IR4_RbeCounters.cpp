/**
 * @file IR4_RbeCounters.cpp
 * @brief v3.12 M3.3 切片4：NOTIFY 门控 RBE 计数暴露（纯逻辑，doc 13 §8.3-5）。
 *
 * ctest 名固定 IR4_RbeCounters（放在 if(INDURTDB_ENABLE_VSOA) 块之外，
 * OFF/ON 构建均编译运行；不占用切片 5 的 IR5_NotifyDirection 名）。
 *
 * 为什么测纯逻辑：OFF 构建不编译 vsoa_adapter.c（依赖 VSOA SDK），且
 * vsoa_notify_dispatch 是 static，无法直接触发真实门控循环。故把「门控 +
 * 计数」抽成传输无关纯函数 rtdbd_session_notify_gate / _deliver_guard，
 * 在本用例里用 fake RBE 回调直接验证 4 个门控分支与 4 个计数出口。
 *
 * 关键回归护栏：rtdbd_dispatch_notify_rbe 内部调 rtdbd_subtab_decide 推进
 * RBE 状态（有副作用），故 SKIP 类（sync_peer / 未订阅 / NULL 会话指针）绝不能
 * 触碰 RBE 回调。RbeCallbackNotInvokedForSkipped 用 spy.calls==0 锁住该顺序。
 * 注：adapter 层的「空槽」在 vsoa_notify_dispatch 循环里已 continue，根本不会
 * 进入 gate；本文件用 s=nullptr 测的只是「NULL 会话指针防御」分支（评审 M-7）。
 *
 * 留白：真实 VSOA 订阅端到端、snapshot_done_caps_only 生产埋点不在本用例
 * 覆盖范围（见变更卡「留白」节）。
 */
#include <gtest/gtest.h>

#include "rtdbd_session.h" /* 自带 extern "C" */

#include <cstdint>

namespace {

constexpr uint32_t kPoint = 42u;

/* fake RBE：pass 决定门控是否命中；calls 用于断言「RBE 是否被提前调用」。 */
struct RbeSpy {
    bool pass;
    int  calls;
};

bool fake_rbe(void *ctx, rtdbd_session_t *s, uint32_t point_id)
{
    RbeSpy *spy = static_cast<RbeSpy *>(ctx);
    (void)s;
    (void)point_id;
    spy->calls++;
    return spy->pass;
}

/* RAII 会话：init/free 配对，禁止拷贝（rtdbd_subtab_t 内含 slots 指针，
 * 浅拷贝会双重释放）。subscribed 时用公开 API 建立「有序视图 + 合并表」，
 * 使 rtdbd_session_subs_contains 走真实合并表判定。 */
struct SessionGuard {
    rtdbd_session_t s{};
    bool            ready = false;

    SessionGuard(bool subscribed, rtdbd_session_role_t role)
    {
        if (!rtdbd_session_init(&s, RTDBD_SUBTAB_MIN_CAP)) {
            return;
        }
        s.role = role;
        if (subscribed) {
            ready = (rtdbd_session_subs_add(&s, kPoint, RTDBD_SUB_MAX)
                     == RTDBD_SUBS_ADD_OK);
        } else {
            ready = true;
        }
    }
    ~SessionGuard() { rtdbd_session_free(&s); }
    SessionGuard(const SessionGuard &) = delete;
    SessionGuard &operator=(const SessionGuard &) = delete;
};

/* 已订阅 + RBE 通过 → HIT，hits+1，其余计数保持 0，RBE 恰调用 1 次。
 * leak 断言改为「串联 guard 之后」：guard 是唯一写 peer_notify_leak 的出口，
 * 在 guard 调用之后断言才有信息量（评审 M-3）。 */
TEST(RbeCounters, SubscribedAndRbePassRecordsHit)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    RbeSpy spy{true, 0};

    rtdbd_notify_gate_t gate =
        rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c);

    EXPECT_EQ(gate, RTDBD_NOTIFY_GATE_HIT);
    EXPECT_EQ(c.rbe_gate_hits, 1u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_TRUE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, &c));
    EXPECT_EQ(c.peer_notify_leak, 0u); /* 串联 guard 之后断言，非恒真 */
    EXPECT_EQ(spy.calls, 1);
}

/* 已订阅 + RBE 拒绝 → MISS，misses+1，hits 保持 0，RBE 恰调用 1 次。
 * gate 从不写 peer_notify_leak，过去在此直接断言 leak==0 属恒真弱断言；
 * 生产路径 MISS 后 continue、不进 guard，故此处不设恒真断言，leak 出口由
 * GateHitChainsToGuardWithNoLeak 在串联 guard 之后验证（评审 M-3）。 */
TEST(RbeCounters, SubscribedButRbeRejectRecordsMiss)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    RbeSpy spy{false, 0};

    rtdbd_notify_gate_t gate =
        rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c);

    EXPECT_EQ(gate, RTDBD_NOTIFY_GATE_MISS);
    EXPECT_EQ(c.rbe_gate_misses, 1u);
    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(spy.calls, 1);
}

/* 未订阅 → SKIP，gate 不写任何计数，RBE 不得被调用。
 * 注：gate 从不写 peer_notify_leak，过去在此断言 leak==0 属恒真弱断言
 * （评审 M-3）；生产路径 SKIP 后 continue、不进 guard。leak 出口改由
 * GateHitChainsToGuardWithNoLeak 在串联 guard 之后验证。 */
TEST(RbeCounters, UnsubscribedSkipsAndNoLeak)
{
    SessionGuard g(false, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    RbeSpy spy{true, 0};

    rtdbd_notify_gate_t gate =
        rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c);

    EXPECT_EQ(gate, RTDBD_NOTIFY_GATE_SKIP);
    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);
    EXPECT_EQ(spy.calls, 0); /* 未订阅不得消耗 RBE 状态 */
}

/* sync_peer 即使已订阅也 SKIP（防回环），gate 3 个计数全 0。
 * peer_notify_leak 由 guard 写、gate 从不写，恒真断言已移除（评审 M-3），
 * 改由组合用例在串联 guard 之后验证。 */
TEST(RbeCounters, SyncPeerSkipsEvenIfSubscribed)
{
    SessionGuard g(true, RTDBD_ROLE_SYNC_PEER);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    RbeSpy spy{true, 0};

    rtdbd_notify_gate_t gate =
        rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c);

    EXPECT_EQ(gate, RTDBD_NOTIFY_GATE_SKIP);
    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);
    EXPECT_EQ(spy.calls, 0);
}

/* 锁副作用顺序：所有 SKIP 场景（sync_peer / 未订阅 / NULL 会话指针）都不得
 * 调用 RBE 回调，否则 RBE 状态被未订阅或回环连接消耗（回归）。
 * 注：s=nullptr 是「NULL 会话指针防御」分支，不是 adapter 层的「空槽」——
 * 空槽在 vsoa_notify_dispatch 循环里已 continue，根本不进 gate（评审 M-7）。 */
TEST(RbeCounters, RbeCallbackNotInvokedForSkipped)
{
    {
        SessionGuard g(true, RTDBD_ROLE_SYNC_PEER); /* 已订阅但为回环角色 */
        ASSERT_TRUE(g.ready);
        rtdbd_notify_counters_t c{};
        RbeSpy spy{true, 0};
        EXPECT_EQ(rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c),
                  RTDBD_NOTIFY_GATE_SKIP);
        EXPECT_EQ(spy.calls, 0);
    }
    {
        SessionGuard g(false, RTDBD_ROLE_PLC); /* 未订阅 */
        ASSERT_TRUE(g.ready);
        rtdbd_notify_counters_t c{};
        RbeSpy spy{true, 0};
        EXPECT_EQ(rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c),
                  RTDBD_NOTIFY_GATE_SKIP);
        EXPECT_EQ(spy.calls, 0);
    }
    {
        rtdbd_notify_counters_t c{};
        RbeSpy spy{true, 0};
        EXPECT_EQ(rtdbd_session_notify_gate(nullptr, kPoint, fake_rbe, &spy, &c),
                  RTDBD_NOTIFY_GATE_SKIP); /* NULL 会话指针防御（非空槽） */
        EXPECT_EQ(spy.calls, 0);
    }
}

/* 发送点护栏：非法会话（sync_peer / 未订阅）→ false + leak+1 并拒发；
 * 合法会话 → true 且 leak 不变。正常门控路径下 leak 恒 0。
 * 注：guard 正常路径不可达（gate 已过滤），仅作防御 + 使 peer_notify_leak
 * 出口可观测；本用例绕过 gate 喂非法会话，只验证护栏本身（评审 M-4：保留）。 */
TEST(RbeCounters, DeliverGuardCatchesLeak)
{
    rtdbd_notify_counters_t c{};
    {
        SessionGuard g(true, RTDBD_ROLE_SYNC_PEER);
        ASSERT_TRUE(g.ready);
        EXPECT_FALSE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, &c));
    }
    EXPECT_EQ(c.peer_notify_leak, 1u);
    {
        SessionGuard g(false, RTDBD_ROLE_PLC);
        ASSERT_TRUE(g.ready);
        EXPECT_FALSE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, &c));
    }
    EXPECT_EQ(c.peer_notify_leak, 2u);
    {
        SessionGuard g(true, RTDBD_ROLE_PLC); /* 合法：不记泄露 */
        ASSERT_TRUE(g.ready);
        EXPECT_TRUE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, &c));
    }
    EXPECT_EQ(c.peer_notify_leak, 2u);
}

/* 组合用例（评审 M-3）：合法订阅 + RBE pass → gate 返回 HIT → 调 guard →
 * 返回 true 且串联 guard 之后 leak 仍为 0。这是生产真实序列，leak 断言
 * 出现在 guard 之后（唯一写 leak 的出口），有信息量。 */
TEST(RbeCounters, GateHitChainsToGuardWithNoLeak)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    RbeSpy spy{true, 0};

    rtdbd_notify_gate_t gate =
        rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, &c);
    ASSERT_EQ(gate, RTDBD_NOTIFY_GATE_HIT);
    EXPECT_EQ(c.rbe_gate_hits, 1u);
    EXPECT_EQ(spy.calls, 1);

    EXPECT_TRUE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, &c));
    EXPECT_EQ(c.peer_notify_leak, 0u); /* 串联 guard 之后，有信息量 */
}

/* I-4：ctr 只是诊断出口，==NULL 不得改变业务控制流。gate 仍应正常判定并
 * 返回 HIT / MISS（不得 SKIP），RBE 回调照常调用。 */
TEST(RbeCounters, NullCountersGateStillDecidesHit)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    RbeSpy spy{true, 0};
    EXPECT_EQ(rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, nullptr),
              RTDBD_NOTIFY_GATE_HIT);
    EXPECT_EQ(spy.calls, 1);
}

TEST(RbeCounters, NullCountersGateStillDecidesMiss)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    RbeSpy spy{false, 0};
    EXPECT_EQ(rtdbd_session_notify_gate(&g.s, kPoint, fake_rbe, &spy, nullptr),
              RTDBD_NOTIFY_GATE_MISS);
    EXPECT_EQ(spy.calls, 1);
}

/* I-4：guard 在 ctr==NULL 时仍应允许合法会话发送（不得一刀切拒发）。 */
TEST(RbeCounters, NullCountersGuardAllowsValidSession)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    EXPECT_TRUE(rtdbd_session_notify_deliver_guard(&g.s, kPoint, nullptr));
}

/* M-1：rbe==NULL 属调用方契约违约 → 返回 SKIP 且不触碰任何计数
 * （不得伪装成 RBE 死区抑制而计 rbe_gate_misses）。 */
TEST(RbeCounters, NullRbeIsContractViolationSkipsWithoutCounting)
{
    SessionGuard g(true, RTDBD_ROLE_PLC);
    ASSERT_TRUE(g.ready);
    rtdbd_notify_counters_t c{};
    EXPECT_EQ(rtdbd_session_notify_gate(&g.s, kPoint, nullptr, nullptr, &c),
              RTDBD_NOTIFY_GATE_SKIP);
    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);
    EXPECT_EQ(c.peer_notify_leak, 0u);
}

/* NULL 契约：snapshot(NULL, ...) / snapshot(..., NULL) / reset(NULL) /
 * record(NULL) 不崩溃（锁既有 NULL 防御，回归护栏）。这些防御在 HEAD 原版
 * 已存在，本用例不改变生产行为，属补强，故无需单独 RED。 */
TEST(RbeCounters, CountersApisTolerateNullPointers)
{
    rtdbd_notify_counters_t out{};
    out.rbe_gate_hits = 7u; /* 哨兵：snapshot(NULL, out) 应清零而非保留 */
    rtdbd_notify_counters_snapshot(nullptr, &out);
    EXPECT_EQ(out.rbe_gate_hits, 0u);
    EXPECT_EQ(out.rbe_gate_misses, 0u);
    EXPECT_EQ(out.snapshot_done_caps_only, 0u);
    EXPECT_EQ(out.peer_notify_leak, 0u);

    rtdbd_notify_counters_t src{};
    src.rbe_gate_hits = 5u;
    rtdbd_notify_counters_snapshot(&src, nullptr); /* out==NULL：安全返回 */
    EXPECT_EQ(src.rbe_gate_hits, 5u);              /* 源未被改动 */

    rtdbd_notify_counters_reset(nullptr);                           /* 不崩溃 */
    rtdbd_notify_counters_record_snapshot_done_caps_only(nullptr); /* 不崩溃 */
}

/* reset 把全部 4 项清零。 */
TEST(RbeCounters, ResetYieldsZero)
{
    rtdbd_notify_counters_t c{};
    c.rbe_gate_hits = 7u;
    c.rbe_gate_misses = 8u;
    c.snapshot_done_caps_only = 9u;
    c.peer_notify_leak = 10u;

    rtdbd_notify_counters_reset(&c);

    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);
    EXPECT_EQ(c.peer_notify_leak, 0u);
}

/* snapshot 是逐字段值拷贝（非别名）：改源不影响快照。 */
TEST(RbeCounters, SnapshotRoundtrip)
{
    rtdbd_notify_counters_t c{};
    c.rbe_gate_hits = 3u;
    c.rbe_gate_misses = 5u;
    c.snapshot_done_caps_only = 7u;
    c.peer_notify_leak = 11u;

    rtdbd_notify_counters_t out{};
    rtdbd_notify_counters_snapshot(&c, &out);

    EXPECT_EQ(out.rbe_gate_hits, 3u);
    EXPECT_EQ(out.rbe_gate_misses, 5u);
    EXPECT_EQ(out.snapshot_done_caps_only, 7u);
    EXPECT_EQ(out.peer_notify_leak, 11u);

    c.rbe_gate_hits = 99u;
    EXPECT_EQ(out.rbe_gate_hits, 3u); /* 非别名 */
}

/* record_snapshot_done_caps_only 纯逻辑 +1，其他字段不变（生产路径留白）。 */
TEST(RbeCounters, RecordSnapshotDoneCapsOnly)
{
    rtdbd_notify_counters_t c{};
    EXPECT_EQ(c.snapshot_done_caps_only, 0u);

    rtdbd_notify_counters_record_snapshot_done_caps_only(&c);
    EXPECT_EQ(c.snapshot_done_caps_only, 1u);
    rtdbd_notify_counters_record_snapshot_done_caps_only(&c);
    EXPECT_EQ(c.snapshot_done_caps_only, 2u);

    EXPECT_EQ(c.rbe_gate_hits, 0u);
    EXPECT_EQ(c.rbe_gate_misses, 0u);
    EXPECT_EQ(c.peer_notify_leak, 0u);
}

} // namespace
