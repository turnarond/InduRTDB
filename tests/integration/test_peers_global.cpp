/** v3.12 M3.3：成员表进程级单例 + self_addr 校验（纯逻辑）。 */
#include <gtest/gtest.h>

#include "cluster/rtdbd_peers_global.h"

#include <cstring>

TEST(PeersGlobal, SingleInstanceAcrossCalls)
{
    rtdbd_peer_table_t *a = rtdbd_peers_global();
    rtdbd_peer_table_t *b = rtdbd_peers_global();
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b);  /* 两前端必须拿到同一张表 */
}

TEST(PeersGlobal, SelfAddrValidatedAndNeverPartiallyWritten)
{
    ASSERT_TRUE(rtdbd_peers_set_self_addr("10.9.9.9"));
    EXPECT_STREQ(rtdbd_peers_self_addr(), "10.9.9.9");

    EXPECT_FALSE(rtdbd_peers_set_self_addr("bogus"));
    EXPECT_FALSE(rtdbd_peers_set_self_addr(""));
    EXPECT_FALSE(rtdbd_peers_set_self_addr("999.1.1.1"));
    EXPECT_FALSE(rtdbd_peers_set_self_addr(nullptr));
    EXPECT_STREQ(rtdbd_peers_self_addr(), "10.9.9.9");  /* 失败不改写旧值 */
}

TEST(PeersGlobal, SelfAddrRejectsOwnAddressInTableSet)
{
    ASSERT_TRUE(rtdbd_peers_set_self_addr("10.9.9.9"));
    rtdbd_peer_table_t *t = rtdbd_peers_global();
    rtdbd_peer_table_init(t);

    rtdbd_peer_t p;
    std::memset(&p, 0, sizeof(p));
    std::strcpy(p.addr, "10.9.9.9");
    std::strcpy(p.node_id, "self");
    EXPECT_EQ(rtdbd_peer_table_set(t, &p, 1, rtdbd_peers_self_addr()), -1);
    EXPECT_EQ(t->count, 0u);  /* 失败旧表不变 */

    std::strcpy(p.addr, "10.9.9.10");
    EXPECT_EQ(rtdbd_peer_table_set(t, &p, 1, rtdbd_peers_self_addr()), 0);
    EXPECT_EQ(t->count, 1u);
}
