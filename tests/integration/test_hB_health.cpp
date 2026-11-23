/**
 * @file test_hB_health.cpp
 * @brief h-B：health 镜像共享**真纯函数**逐字节一致 + 异常态（t-E）。
 *
 * 给定完全相同输入（health/8 计数/n_conns/conn_table_ok），两次调用产出
 * 72B 逐字节一致（模拟 UDS/VSOA 两前端喂相同输入）。uptime 是注入值、
 * 非函数内部取时钟——这是"逐字节一致"可证伪的关键（首轮评审 P0-2 修正）。
 */
#include <gtest/gtest.h>

extern "C" {
#include "dispatch/rtdbd_dispatch.h"
}

#include <cstring>

namespace {

static rtdbd_health_counts_t fixed_counts()
{
    rtdbd_health_counts_t c;
    std::memset(&c, 0, sizeof(c));
    c.n_writes = 11u; c.n_notifies = 22u; c.decode_fail = 3u;
    c.write_rejected = 4u; c.write_error = 5u; c.notify_drop = 6u;
    c.notify_send_fail = 7u; c.uptime_ns = 1234567890ull;
    return c;
}

TEST(HBHealth, SameInputProducesByteIdentical72B)
{
    auto counts = fixed_counts();
    rtdbd_health_t a, b;
    std::memset(&a, 0xAA, sizeof(a));
    std::memset(&b, 0x55, sizeof(b));
    rtdbd_dispatch_health_fill(RTDBD_HEALTH_OK, &counts, 3u, true, &a);
    rtdbd_dispatch_health_fill(RTDBD_HEALTH_OK, &counts, 3u, true, &b);
    EXPECT_EQ(std::memcmp(&a, &b, sizeof(rtdbd_health_t)), 0); // 两前端字节一致
    EXPECT_EQ(a.n_writes, 11u);
    EXPECT_EQ(a.uptime_ns, 1234567890ull);
    EXPECT_EQ(a.n_conns, 3u);
    EXPECT_EQ(a.health, (uint32_t)RTDBD_HEALTH_OK);
}

TEST(HBHealth, ConnTableAbnormalRaisesUnhealthy)
{
    auto counts = fixed_counts();
    rtdbd_health_t ok, bad;
    rtdbd_dispatch_health_fill(RTDBD_HEALTH_OK, &counts, 3u, true, &ok);
    rtdbd_dispatch_health_fill(RTDBD_HEALTH_OK, &counts, 3u, false, &bad);
    EXPECT_EQ(ok.health, (uint32_t)RTDBD_HEALTH_OK);
    EXPECT_EQ(bad.health, (uint32_t)RTDBD_HEALTH_UNHEALTHY);
    // 除 health 字段外其余字节一致（输入相同、仅 conn_table_ok 不同）
    EXPECT_EQ(std::memcmp(&ok.n_writes, &bad.n_writes, 64u), 0); // 8×uint64 计数区
}

TEST(HBHealth, SizeIs72B)
{
    EXPECT_EQ(sizeof(rtdbd_health_t), 72u);
}

} // namespace
