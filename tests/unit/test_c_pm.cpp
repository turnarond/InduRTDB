/**
 * @file test_c_pm.cpp
 * @brief PointManager 单元测试
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "osal/irt_osal.h"
#include "core/irt_shm.h"
#include "core/irt_point_manager.h"
}

class CPmTest : public ::testing::Test {
protected:
    void SetUp() override {
        std::memset(&shm_, 0, sizeof(shm_));
        ASSERT_EQ(irt_shm_init(&shm_, "test4", 32, 4), 0);
        irt_pm_init(&pm_, &shm_);
    }
    void TearDown() override {
        irt_shm_shutdown(&shm_);
    }
    irt_shm_t shm_;
    irt_pm_t  pm_;
};

TEST_F(CPmTest, WriteAndReadBool) {
    ASSERT_EQ(irt_pm_write_bool(&pm_, 0, true), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 0, &p), 0);
    EXPECT_EQ(p.value.b, true);
    EXPECT_EQ(p.type, INDURTDB_TYPE_BOOL);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
    EXPECT_GT(p.timestamp_ns, 0u);
}

TEST_F(CPmTest, WriteAndReadInt32) {
    ASSERT_EQ(irt_pm_write_int32(&pm_, 1, -42), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 1, &p), 0);
    EXPECT_EQ(p.value.i, -42);
    EXPECT_EQ(p.type, INDURTDB_TYPE_INT32);
}

TEST_F(CPmTest, WriteAndReadDouble) {
    ASSERT_EQ(irt_pm_write_double(&pm_, 2, 3.14), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 2, &p), 0);
    EXPECT_DOUBLE_EQ(p.value.d, 3.14);
    EXPECT_EQ(p.type, INDURTDB_TYPE_DOUBLE);
}

TEST_F(CPmTest, WriteAndReadString) {
    ASSERT_EQ(irt_pm_write_string(&pm_, 3, "hello"), 0);
    indurtdb_point_t p;
    ASSERT_EQ(irt_pm_read(&pm_, 3, &p), 0);
    EXPECT_STREQ(p.value.str, "hello");
    EXPECT_EQ(p.type, INDURTDB_TYPE_STRING);
}

TEST_F(CPmTest, PeekZeroCopy) {
    ASSERT_EQ(irt_pm_write_int32(&pm_, 5, 99), 0);
    const indurtdb_point_t* pp = irt_pm_peek(&pm_, 5);
    ASSERT_NE(pp, nullptr);
    EXPECT_EQ(pp->value.i, 99);
    /* peek 返回的是共享内存指针, 不是副本 */
}

TEST_F(CPmTest, InvalidIdRejected) {
    EXPECT_FALSE(irt_pm_validate_id(&pm_, 32));  /* max_points=32, id 32 越界 */
    EXPECT_EQ(irt_pm_write_int32(&pm_, 32, 1), -1);
    indurtdb_point_t p;
    EXPECT_EQ(irt_pm_read(&pm_, 32, &p), -1);
    EXPECT_EQ(irt_pm_peek(&pm_, 32), nullptr);
}

TEST_F(CPmTest, WriteCountIncrements) {
    uint64_t c0 = irt_pm_write_count(&pm_);
    irt_pm_write_bool(&pm_, 10, true);
    irt_pm_write_int32(&pm_, 11, 1);
    uint64_t c1 = irt_pm_write_count(&pm_);
    EXPECT_EQ(c1, c0 + 2);
}
