/**
 * @file test_c_shm.cpp
 * @brief 共享内存段单元测试
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "core/irt_shm.h"
}

class CShmTest : public ::testing::Test {
protected:
    void TearDown() override {
        if (inited_) { irt_shm_shutdown(&shm_); inited_ = false; }
    }
    irt_shm_t shm_;
    bool      inited_ = false;
};

TEST_F(CShmTest, InitOwner) {
    std::memset(&shm_, 0, sizeof(shm_));
    int rc = irt_shm_init(&shm_, "test3", 64, 8);
    ASSERT_EQ(rc, 0);
    inited_ = true;

    EXPECT_TRUE(irt_shm_is_owner(&shm_));
    EXPECT_NE(irt_shm_header(&shm_), nullptr);
    EXPECT_NE(irt_shm_points(&shm_), nullptr);
    EXPECT_NE(irt_shm_subscribers(&shm_), nullptr);

    irt_header_t* hdr = irt_shm_header(&shm_);
    EXPECT_EQ(hdr->magic, IRT_MAGIC);
    EXPECT_EQ(hdr->version, IRT_SHM_VERSION);
    EXPECT_EQ(hdr->max_points, 64u);
    EXPECT_EQ(hdr->max_subscribers, 8u);

    /* 地址计算: 一律按 Header 中的区段偏移, 不按 sizeof 硬算。
     * v3.4 布局 v2 起点位区与心跳区之间还有索引区(以及后续元数据区)。 */
    EXPECT_EQ((char*)irt_shm_points(&shm_) - (char*)hdr,
              (ptrdiff_t)hdr->off_points);
    EXPECT_EQ((char*)irt_shm_subscribers(&shm_) - (char*)hdr,
              (ptrdiff_t)hdr->off_subs);
    /* 心跳区起点 > 点位区末尾: 差值即索引区(当前) + 元数据区(后续) */
    EXPECT_GT((size_t)hdr->off_subs,
              (size_t)hdr->off_points + 64u * sizeof(indurtdb_point_t));
}

TEST_F(CShmTest, AttachNotOwner) {
    /* 第一个实例: owner */
    irt_shm_t owner_shm;
    std::memset(&owner_shm, 0, sizeof(owner_shm));
    ASSERT_EQ(irt_shm_init(&owner_shm, "test3b", 32, 4), 0);

    /* 第二个实例: attach, 非 owner */
    std::memset(&shm_, 0, sizeof(shm_));
    int rc = irt_shm_init(&shm_, "test3b", 32, 4);
    ASSERT_EQ(rc, 0);
    inited_ = true;

    EXPECT_FALSE(irt_shm_is_owner(&shm_));
    EXPECT_EQ(irt_shm_header(&shm_)->magic, IRT_MAGIC);
    EXPECT_EQ(irt_shm_header(&shm_)->max_points, 32u);

    irt_shm_shutdown(&owner_shm);
}

TEST_F(CShmTest, InitZeroSubscribers) {
    /* max_subscribers=0 允许: 段仅含 header + points, 无订阅者表 */
    std::memset(&shm_, 0, sizeof(shm_));
    int rc = irt_shm_init(&shm_, "test3c", 16, 0);
    ASSERT_EQ(rc, 0);
    inited_ = true;

    EXPECT_TRUE(irt_shm_is_owner(&shm_));
    EXPECT_NE(irt_shm_header(&shm_), nullptr);
    EXPECT_NE(irt_shm_points(&shm_), nullptr);
    EXPECT_EQ(irt_shm_subscribers(&shm_), nullptr);  /* 0 订阅者 → 无表 */

    irt_header_t* hdr = irt_shm_header(&shm_);
    EXPECT_EQ(hdr->max_subscribers, 0u);

    /* 总大小 = header + points + 索引区 + 元数据区(均定长, 随段预分配) */
    EXPECT_EQ(irt_shm_total_size(16, 0),
              sizeof(irt_header_t) + 16 * sizeof(indurtdb_point_t)
              + irt_layout_index_size(16)
              + irt_layout_meta_size(16));
}

TEST_F(CShmTest, TotalSizeFormula) {
    /* 与 v2.x 公式一致 */
    size_t sz = irt_shm_total_size(100, 16);
    EXPECT_EQ(sz, sizeof(irt_header_t)
                + 100 * sizeof(indurtdb_point_t)
                + irt_layout_index_size(100)
                + irt_layout_meta_size(100)
                + 16  * sizeof(irt_subscriber_entry_t));
    /* IRT_STATIC_ASSERT 已保证各 struct 大小; 索引区 = 桶数(256) × 8B = 2048,
     * 元数据区 = 100 × 32B = 3200
     * v3.4 布局 v2: 128 + 100*128 + 2048(index) + 3200(meta) + 16*16(subs) */
    EXPECT_EQ(sz, 128u + 12800u + 2048u + 3200u + 256u);
}
