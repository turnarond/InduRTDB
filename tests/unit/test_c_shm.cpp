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

    /* 地址计算: points 在 header 之后, subscribers 在 points 之后 */
    EXPECT_EQ((char*)irt_shm_points(&shm_) - (char*)hdr,
              (ptrdiff_t)sizeof(irt_header_t));
    EXPECT_EQ((char*)irt_shm_subscribers(&shm_)
              - (char*)irt_shm_points(&shm_),
              (ptrdiff_t)(64 * sizeof(indurtdb_point_t)));
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

    /* 总大小仅 header + points */
    EXPECT_EQ(irt_shm_total_size(16, 0),
              sizeof(irt_header_t) + 16 * sizeof(indurtdb_point_t));
}

TEST_F(CShmTest, TotalSizeFormula) {
    /* 与 v2.x 公式一致 */
    size_t sz = irt_shm_total_size(100, 16);
    EXPECT_EQ(sz, sizeof(irt_header_t)
                + 100 * sizeof(indurtdb_point_t)
                + 16  * sizeof(irt_subscriber_entry_t));
    /* IRT_STATIC_ASSERT 已保证各 struct 大小: 64 + 100*128 + 16*16 = 64+12800+256 */
    EXPECT_EQ(sz, 64u + 12800u + 256u);
}
