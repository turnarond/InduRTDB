/**
 * @file test_hC_readonly.cpp
 * @brief h-C 前置：4 个只读 URL 共享纯函数（OFF 可跑，仿 test_hB_health.cpp）。
 *
 * 固定注入输入断言逐字节一致（两前端一致的可证伪代理）+ 分区白名单/越界/
 * max_points 注入/日志截断/name NUL 负例。ON 侧真实 URL+e2e 由 test_vsoa_e2e 覆盖。
 */
#include <gtest/gtest.h>

extern "C" {
#include "dispatch/rtdbd_dispatch.h"
#include "rtdbd_conf.h"
}

#include <cstring>
#include <string>

namespace {

class HcReadonlyTest : public ::testing::Test {
protected:
    indurtdb_t *h = nullptr;

    void SetUp() override {
        // 每个 TEST 用唯一实例名，避免 shm 段复用导致点数串扰。
        static int seq = 0;
        indurtdb_cfg_t cfg{};
        cfg.max_points = 64;
        cfg.max_subscribers = 4;
        char inst[64];
        std::snprintf(inst, sizeof(inst), "hc_ro_%d_%d", static_cast<int>(getpid()), ++seq);
        ASSERT_EQ(indurtdb_h_open(&h, inst, &cfg), 0);
        ASSERT_EQ(indurtdb_h_create_point(h, 5, "p5", INDURTDB_TYPE_INT32,
                                          INDURTDB_ACCESS_READ_WRITE), 0);
        ASSERT_EQ(indurtdb_h_create_point(h, 9, "p9", INDURTDB_TYPE_INT32,
                                          INDURTDB_ACCESS_READ_WRITE), 0);
    }
    void TearDown() override {
        if (h) indurtdb_h_close(h);
    }
};

TEST_F(HcReadonlyTest, FindByteIdenticalAndNameNul)
{
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf); // n_partitions=0 ⇒ 任意分区合法

    uint32_t id1 = 0, id2 = 0;
    char name[64];
    std::memset(name, 0, sizeof(name));
    std::memcpy(name, "p5", 2); // 正常 name：NUL 结尾

    uint16_t s1 = rtdbd_dispatch_find(h, &conf, 0, name, &id1);
    uint16_t s2 = rtdbd_dispatch_find(h, &conf, 0, name, &id2);
    EXPECT_EQ(s1, RTDBD_ST_OK);
    EXPECT_EQ(s2, RTDBD_ST_OK);
    EXPECT_EQ(id1, 5u);
    EXPECT_EQ(id2, id1); // 相同输入 → 逐字节一致（id 相同）

    // name 全 0xFF 无 NUL：调用方（UDS/VSOA op）补末尾 '\0' 后传入，不越界、不误匹配
    char nul_name[64];
    std::memset(nul_name, 0xFF, sizeof(nul_name));
    nul_name[sizeof(nul_name) - 1] = '\0';
    uint32_t junk = 999u;
    EXPECT_EQ(rtdbd_dispatch_find(h, &conf, 0, nul_name, &junk), RTDBD_ST_NOT_FOUND);

    // 未找到的点
    uint32_t miss = 999u;
    EXPECT_EQ(rtdbd_dispatch_find(h, &conf, 0, "no_such_point", &miss), RTDBD_ST_NOT_FOUND);
}

TEST_F(HcReadonlyTest, FindPartitionWhitelistFailClosed)
{
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf);
    conf.n_partitions = 1;
    conf.partitions[0] = 0; // 仅分区 0 合法

    uint32_t id = 0;
    EXPECT_EQ(rtdbd_dispatch_find(h, &conf, 0, "p5", &id), RTDBD_ST_OK);
    EXPECT_EQ(rtdbd_dispatch_find(h, &conf, 7, "p5", &id), RTDBD_ST_NOT_FOUND); // 非法分区
}

TEST_F(HcReadonlyTest, MetaByteIdenticalAndArgErrors)
{
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf);

    rtdbd_meta_payload_t m1, m2;
    std::memset(&m1, 0xAA, sizeof(m1));
    std::memset(&m2, 0x55, sizeof(m2));
    EXPECT_EQ(rtdbd_dispatch_meta(h, 5, &m1), RTDBD_ST_OK);
    EXPECT_EQ(rtdbd_dispatch_meta(h, 5, &m2), RTDBD_ST_OK);
    EXPECT_EQ(std::memcmp(&m1, &m2, sizeof(m1)), 0); // 逐字节一致

    rtdbd_meta_payload_t m3;
    EXPECT_EQ(rtdbd_dispatch_meta(h, 9999, &m3), RTDBD_ST_BAD_REQUEST); // 越界 id
}

TEST_F(HcReadonlyTest, ListSnapshotWhitelistAndMaxPointsInjection)
{
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf);
    conf.n_partitions = 1;
    conf.partitions[0] = 0;
    uint8_t buf[8192];
    size_t len1 = 0, len2 = 0;

    // 非法具体分区 → OK + 空 body
    EXPECT_EQ(rtdbd_dispatch_list_snapshot(h, &conf, 7, 0, 0, 64, buf, sizeof(buf), &len1),
              RTDBD_ST_OK);
    EXPECT_EQ(len1, 0u);

    // ALL 哨兵不过滤 → 列出全部已注册点（2 个）
    EXPECT_EQ(rtdbd_dispatch_list_snapshot(h, &conf, RTDBD_PARTITION_ALL, 0, 0, 64,
                                           buf, sizeof(buf), &len1), RTDBD_ST_OK);
    EXPECT_EQ(len1, 2u * sizeof(rtdbd_point_info_t));

    // max_points 注入：注入 6 时扫描 [0,6)、注入 64 扫 [0,64)——本例点都在 <6，
    // 故用一个 >6 的点验证注入生效（另建一个 id=20 的点）
    ASSERT_EQ(indurtdb_h_create_point(h, 20, "p20", INDURTDB_TYPE_INT32,
                                      INDURTDB_ACCESS_READ_WRITE), 0);
    EXPECT_EQ(rtdbd_dispatch_list_snapshot(h, &conf, RTDBD_PARTITION_ALL, 0, 0, 6,
                                           buf, sizeof(buf), &len1), RTDBD_ST_OK);
    EXPECT_EQ(len1, 1u * sizeof(rtdbd_point_info_t)); // 仅 id=5 在 [0,6)；id=9/20 不在
    EXPECT_EQ(rtdbd_dispatch_list_snapshot(h, &conf, RTDBD_PARTITION_ALL, 0, 0, 64,
                                           buf, sizeof(buf), &len2), RTDBD_ST_OK);
    EXPECT_EQ(len2, 3u * sizeof(rtdbd_point_info_t)); // id 20 出现在 [0,64)
}

TEST_F(HcReadonlyTest, ListSnapshotCapTruncation)
{
    rtdbd_conf_t conf;
    rtdbd_conf_init(&conf);
    // 小 cap 只容 1 条：即使 max=100、已注册 2 点，也只返回 1 条。
    uint8_t small[sizeof(rtdbd_point_info_t)];
    size_t len = 0;
    EXPECT_EQ(rtdbd_dispatch_list_snapshot(h, &conf, RTDBD_PARTITION_ALL, 100, 0, 64,
                                           small, sizeof(small), &len), RTDBD_ST_OK);
    EXPECT_EQ(len, sizeof(rtdbd_point_info_t)); // 按 cap=76B 截断到 1 条
}

TEST_F(HcReadonlyTest, LogSnapshotSinglePageAndCap)
{
    irt_logbuf_t lb;
    irt_logbuf_init(&lb);
    for (int i = 0; i < 5; ++i) {
        irt_logbuf_emit(&lb, RTDBD_LOG_INFO, "log-%d", i);
    }
    uint8_t big[RTDBD_LOG_CAPACITY * sizeof(rtdbd_log_entry_t)];
    size_t len = 0;
    EXPECT_EQ(rtdbd_dispatch_log_snapshot(&lb, 0, big, sizeof(big), &len), RTDBD_ST_OK);
    EXPECT_EQ(len, 5u * sizeof(rtdbd_log_entry_t)); // max=0 → 全部

    // max=2 → 仅 2 条（单页）
    EXPECT_EQ(rtdbd_dispatch_log_snapshot(&lb, 2, big, sizeof(big), &len), RTDBD_ST_OK);
    EXPECT_EQ(len, 2u * sizeof(rtdbd_log_entry_t));

    // 小 cap 截断：cap=1 条
    uint8_t small[sizeof(rtdbd_log_entry_t)];
    EXPECT_EQ(rtdbd_dispatch_log_snapshot(&lb, 0, small, sizeof(small), &len), RTDBD_ST_OK);
    EXPECT_EQ(len, sizeof(rtdbd_log_entry_t));
}

} // namespace
