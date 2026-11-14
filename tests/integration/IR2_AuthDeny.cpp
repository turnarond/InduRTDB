/**
 * @file IR2_AuthDeny.cpp
 * @brief v3.12 M3.x / P7 鉴权判据（doc 13 §11.1，固定 ctest 名 IR2_AuthDeny）。
 *
 * 纯逻辑、默认构建可跑：构造角色 / 来源地址 / bare URL 的组合，验证 §8.3-4
 * 的 deny-by-default 矩阵。现场才有的「非 loopback 来源」用构造地址在此真实
 * 判定，不依赖跨机链路；真实 rtdbd 的接线证据见 ON-only test_vsoa_auth.cpp。
 *
 * 覆盖 P7 四个判据字段的语义：
 *   deny_bad_token      → 端到端（test_vsoa_auth.cpp，token 表属 adapter）
 *   deny_peer_write     → SYNC_PEER 不得 /point/write，仅同步前缀 URL
 *   deny_non_loopback   → PLC / SYNC_ADMIN 仅 127/8
 *   admin_set_peers_ok  → SYNC_ADMIN 可 /sync/peers/set|get
 */
#include <gtest/gtest.h>

#include <rtdbd/protocol.h>
#include "session/rtdbd_authz.h"
#include <cstring>
#include <cstdio>

namespace {

constexpr uint32_t kLoopback    = 0x7F000001u; /* 127.0.0.1 主机序 */
constexpr uint32_t kLoopbackHi  = 0x7F112233u; /* 127/8 段内任意地址 */
constexpr uint32_t kRemote      = 0x0A000001u; /* 10.0.0.1 主机序 */
constexpr uint32_t kLinkLocal   = 0xA9FE0001u; /* 169.254.0.1 主机序 */

} // namespace

TEST(IR2_AuthDeny, DenyNonLoopbackForPlcAndSyncAdmin)
{
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_PLC, true, kRemote),
              RTDBD_ST_DENIED);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_SYNC_ADMIN, true, kRemote),
              RTDBD_ST_DENIED);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_PLC, true, kLinkLocal),
              RTDBD_ST_DENIED);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_PLC, false, kLoopback),
              RTDBD_ST_DENIED) << "非 IPv4 来源一律拒绝";
}

TEST(IR2_AuthDeny, AllowOnly127Slash8ForLocalRoles)
{
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_PLC, true, kLoopback),
              RTDBD_ST_OK);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_PLC, true, kLoopbackHi),
              RTDBD_ST_OK);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_SYNC_ADMIN, true, kLoopback),
              RTDBD_ST_OK);
}

TEST(IR2_AuthDeny, SyncPeerSourceAllowedFallsBackToPlaceholder)
{
    /* rtdbd_authz_source_allowed 的 SYNC_PEER 分支是「不可达占位」（真实来源校验
     * 在 rtdbd_authz_sync_peer_source_allowed，需运行时成员表上下文，vsoa_adapter
     * 已特判调用）。此处断言占位显式返回 NOT_IMPLEMENTED，不得默认放行。 */
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_SYNC_PEER, true, kRemote),
              RTDBD_ST_NOT_IMPLEMENTED);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_SYNC_PEER, true, kLoopback),
              RTDBD_ST_NOT_IMPLEMENTED);
    EXPECT_EQ(rtdbd_authz_source_allowed(RTDBD_ROLE_UNKNOWN, true, kLoopback),
              RTDBD_ST_DENIED);
}

TEST(IR2_AuthDeny, DenyPeerWrite)
{
    /* §8.3-4：sync_peer 仅 /sync/ 前缀，禁止写点位 / 订阅（防回环）。 */
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/point/write"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/point/batch_write"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/subscription/add"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/ping"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/sync/replay"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_PEER, "/sync/peers/get"));
}

TEST(IR2_AuthDeny, AdminSetPeersOkButCannotWritePoints)
{
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/sync/peers/set"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/sync/peers/get"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/point/write"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/caps"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/sync/replay"));
}

TEST(IR2_AuthDeny, PlcMayWriteButNeverSyncUrls)
{
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, "/point/write"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, "/ping"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, "/caps"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, "/sync/peers/set"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, "/sync/replay"));
}

TEST(IR2_AuthDeny, DenyByDefaultAndHelloAlwaysReachable)
{
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_UNKNOWN, "/ping"));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, nullptr));
    EXPECT_FALSE(rtdbd_authz_url_allowed(RTDBD_ROLE_PLC, ""));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_UNKNOWN, "/hello"));
    EXPECT_TRUE(rtdbd_authz_url_allowed(RTDBD_ROLE_SYNC_ADMIN, "/hello"));
}

TEST(IR2_AuthDeny, SyncPeerSourceInMemberTable)
{
    /* §8.3-4：sync_peer 来源必须是已知集群成员（落在成员表 addr 集合）。 */
    rtdbd_peer_table_t tbl;
    rtdbd_peer_table_init(&tbl);
    rtdbd_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::snprintf(p.addr, sizeof(p.addr), "127.0.0.1");
    std::snprintf(p.node_id, sizeof(p.node_id), "n0");
    p.state = RTDBD_PEER_ST_ACTIVE;
    EXPECT_EQ(rtdbd_peer_table_add(&tbl, &p, nullptr), 0)
        << "127.0.0.1 应成功加入成员表";

    EXPECT_EQ(rtdbd_authz_sync_peer_source_allowed(&tbl, "127.0.0.1"), RTDBD_ST_OK);
    EXPECT_EQ(rtdbd_authz_sync_peer_source_allowed(&tbl, "10.0.0.9"), RTDBD_ST_DENIED);
    rtdbd_peer_table_t empty;
    rtdbd_peer_table_init(&empty);
    EXPECT_EQ(rtdbd_authz_sync_peer_source_allowed(&empty, "127.0.0.1"), RTDBD_ST_DENIED);
    EXPECT_EQ(rtdbd_authz_sync_peer_source_allowed(nullptr, "127.0.0.1"), RTDBD_ST_DENIED);
    EXPECT_EQ(rtdbd_authz_sync_peer_source_allowed(&tbl, "not-an-ip"), RTDBD_ST_DENIED);
}
