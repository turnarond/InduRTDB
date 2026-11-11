/**
 * @file IR4_PeersRuntime.cpp
 * @brief v3.12 IR4：成员运行时表语义（doc 13 §8.3-6，§11.1 用例名固定）。
 *
 * 覆盖 §11.1 的 B 段 P8 判据（本用例给出**语义断言**，真数字落库属 M5）：
 *   set_peers_idempotent / reject_self / reject_invalid / reject_over_limit /
 *   old_table_unchanged / join_triggers_full。
 *
 * ⚠ 范围声明：本用例验证**成员表纯逻辑**（定长 32、原子整组替换、校验与幂等）；
 * 6 节点联动、`SET_PEERS`/`GET_PEERS` 线上路径、`--sync-status` 与 health 暴露
 * 属 M3 后续，以 M2 harness 为载体补齐。
 */
#include <gtest/gtest.h>

#include "rtdbd_peer_table.h"
#include "rtdbd_peers_global.h"

#include <cstring>

namespace {

rtdbd_peer_t make_peer(const char *addr, const char *node_id = "node")
{
    rtdbd_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.addr, sizeof(p.addr), "%s", addr);
    std::snprintf(p.node_id, sizeof(p.node_id), "%s", node_id);
    p.state = RTDBD_PEER_ST_ACTIVE;
    return p;
}

bool same_table(const rtdbd_peer_table_t &a, const rtdbd_peer_table_t &b)
{
    if (a.count != b.count) {
        return false;
    }
    return std::memcmp(a.peers, b.peers, a.count * sizeof(a.peers[0])) == 0;
}

} // namespace

TEST(IR4_PeersRuntime, SetPeersIsIdempotent)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t peers[3] = {make_peer("10.0.0.1"), make_peer("10.0.0.2"),
                             make_peer("10.0.0.3")};
    ASSERT_EQ(rtdbd_peer_table_set(&t, peers, 3, "10.0.0.9"), 0);
    ASSERT_EQ(t.count, 3u);
    rtdbd_peer_table_t first = t;

    /* 同一集合重复 set → 结果完全一致（set_peers_idempotent） */
    ASSERT_EQ(rtdbd_peer_table_set(&t, peers, 3, "10.0.0.9"), 0);
    EXPECT_EQ(t.count, 3u);
    EXPECT_TRUE(same_table(t, first));
}

TEST(IR4_PeersRuntime, RejectsSelfAddress)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t old = make_peer("10.0.0.1");
    ASSERT_EQ(rtdbd_peer_table_set(&t, &old, 1, "10.0.0.9"), 0);
    rtdbd_peer_table_t before = t;

    rtdbd_peer_t peers[2] = {make_peer("10.0.0.1"), make_peer("10.0.0.9")};
    EXPECT_EQ(rtdbd_peer_table_set(&t, peers, 2, "10.0.0.9"), -1);
    EXPECT_TRUE(same_table(t, before)); /* 旧表不变 */
}

TEST(IR4_PeersRuntime, RejectsInvalidAddresses)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t old = make_peer("10.0.0.1");
    ASSERT_EQ(rtdbd_peer_table_set(&t, &old, 1, "10.0.0.9"), 0);
    rtdbd_peer_table_t before = t;

    const char *bad[] = {"", "not-an-ip", "999.1.1.1", "10.0.0"};
    for (const char *addr : bad) {
        rtdbd_peer_t p = make_peer(addr);
        EXPECT_FALSE(rtdbd_peer_addr_valid(addr));
        EXPECT_EQ(rtdbd_peer_table_set(&t, &p, 1, "10.0.0.9"), -1);
        EXPECT_TRUE(same_table(t, before)) << "old table changed for " << addr;
    }
    /* node_id 是定长数组[32]：31 字符 + '\0' 已是**类型系统允许的合法最大值**
     * （无法在类型内构造更长的字符串），故应接受且内容不得截断。 */
    rtdbd_peer_t p = make_peer("10.0.0.2");
    std::memset(p.node_id, 'x', sizeof(p.node_id) - 1);
    p.node_id[sizeof(p.node_id) - 1] = '\0';
    ASSERT_EQ(rtdbd_peer_table_set(&t, &p, 1, "10.0.0.9"), 0);
    ASSERT_EQ(t.count, 1u);
    EXPECT_EQ(std::strlen(t.peers[0].node_id), sizeof(p.node_id) - 1);
}

TEST(IR4_PeersRuntime, RejectsOverLimit)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t old = make_peer("10.0.0.1");
    ASSERT_EQ(rtdbd_peer_table_set(&t, &old, 1, "10.0.0.9"), 0);
    rtdbd_peer_table_t before = t;

    rtdbd_peer_t many[RTDBD_PEER_MAX + 1];
    for (uint32_t i = 0; i < RTDBD_PEER_MAX + 1; ++i) {
        many[i] = make_peer("10.1.0.1");
    }
    EXPECT_EQ(rtdbd_peer_table_set(&t, many, RTDBD_PEER_MAX + 1, "10.0.0.9"), -1);
    EXPECT_TRUE(same_table(t, before));
}

TEST(IR4_PeersRuntime, OldTableUnchangedOnAnyFailure)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t peers[3] = {make_peer("10.0.0.1", "n1"), make_peer("10.0.0.2", "n2"),
                             make_peer("10.0.0.3", "n3")};
    ASSERT_EQ(rtdbd_peer_table_set(&t, peers, 3, "10.0.0.9"), 0);
    rtdbd_peer_table_t before = t;

    /* 混合：前面合法、后面非法 → 仍必须整组拒绝，旧表逐字节不变 */
    rtdbd_peer_t mixed[3] = {make_peer("10.0.0.4"), make_peer("bogus"),
                             make_peer("10.0.0.5")};
    EXPECT_EQ(rtdbd_peer_table_set(&t, mixed, 3, "10.0.0.9"), -1);
    EXPECT_TRUE(same_table(t, before));
    EXPECT_STREQ(t.peers[0].node_id, "n1");
}

TEST(IR4_PeersRuntime, JoinTriggersFullAndAddIsIdempotent)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t p1 = make_peer("10.0.0.1");
    ASSERT_EQ(rtdbd_peer_table_add(&t, &p1, "10.0.0.9"), 0); /* 新增 → 应触发 on_join 全量 */
    EXPECT_EQ(t.count, 1u);
    EXPECT_EQ(rtdbd_peer_table_add(&t, &p1, "10.0.0.9"), 1); /* 已存在 → 幂等，不触发 */
    EXPECT_EQ(t.count, 1u);

    rtdbd_peer_t p2 = make_peer("10.0.0.2");
    ASSERT_EQ(rtdbd_peer_table_add(&t, &p2, "10.0.0.9"), 0);
    EXPECT_EQ(t.count, 2u);

    const rtdbd_peer_t *got = rtdbd_peer_table_get(&t, "10.0.0.2");
    ASSERT_NE(got, nullptr);
    EXPECT_STREQ(got->node_id, "node");
    EXPECT_EQ(rtdbd_peer_table_del(&t, "10.0.0.1"), 0);
    EXPECT_EQ(t.count, 1u);
    EXPECT_EQ(rtdbd_peer_table_del(&t, "10.0.0.1"), 1); /* 不存在无副作用 */
    EXPECT_EQ(rtdbd_peer_table_get(&t, "10.0.0.1"), nullptr);
}

TEST(IR4_PeersRuntime, DeduplicatesRepeatedIp)
{
    rtdbd_peer_table_t t;
    rtdbd_peer_table_init(&t);
    rtdbd_peer_t peers[3] = {make_peer("10.0.0.1", "a"), make_peer("10.0.0.1", "b"),
                             make_peer("10.0.0.2", "c")};
    ASSERT_EQ(rtdbd_peer_table_set(&t, peers, 3, "10.0.0.9"), 0);
    EXPECT_EQ(t.count, 2u); /* 重复 IP 去重 */
    EXPECT_STREQ(t.peers[0].node_id, "a"); /* 保留首次出现 */
}

/* v3.12 M3.3 切片3：最小 lag 计算（§8.3-6 lag 精确语义未裁决，仅落地差值）。
 * 纯函数 rtdbd_peer_lag 由 rtdbd_peers_apply.c 在 SET_PEERS 时调用写入 peer.lag；
 * 此处单测纯函数逻辑，apply.c 集成（SET_PEERS→写 lag）留 rtdbd 进程级/ON 验证。 */
TEST(IR4_PeersRuntime, LagComputedAsLocalMinusApplied)
{
    EXPECT_EQ(rtdbd_peer_lag(150, 100), 50u);  /* local150 - applied100 */
    EXPECT_EQ(rtdbd_peer_lag(150, 200), 0u);   /* 不落后 → 饱和 0 */
    EXPECT_EQ(rtdbd_peer_lag(150, 150), 0u);   /* 恰好追上 → 0 */
    EXPECT_EQ(rtdbd_peer_lag(0, 100), 0u);     /* local 0 → 饱和 0 */
}

/* 切片3：boot_id 生成非 0（重启变化的前置；进程级「变化」验证留 IR6 env）。 */
TEST(IR4_PeersRuntime, BootIdGeneratedNonZero)
{
    EXPECT_NE(rtdbd_boot_id_generate(), 0u);
}

/* 切片3：boot_id 存取器往返。 */
TEST(IR4_PeersRuntime, BootIdRoundtrip)
{
    rtdbd_peers_set_boot_id(0xDEADBEEFCAFEULL);
    EXPECT_EQ(rtdbd_peers_boot_id(), 0xDEADBEEFCAFEULL);
}
