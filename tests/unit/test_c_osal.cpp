/**
 * @file test_c_osal.cpp
 * @brief OSAL C 实现单元测试
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include "osal/irt_osal.h"
}

TEST(COsalTime, MonotonicNowNs) {
    uint64_t t1 = irt_time_now_ns();
    ASSERT_GT(t1, 0u);
    irt_time_sleep_ns(1000000ULL);      /* 1 ms */
    uint64_t t2 = irt_time_now_ns();
    EXPECT_GT(t2, t1);
}

TEST(COsalShm, MapOwnerAndAttach) {
    irt_shm_os_t owner;
    std::memset(&owner, 0, sizeof(owner));
    void* base = irt_shm_os_map(&owner, "/irt_test_osal", 4096);
    ASSERT_NE(base, nullptr);
    EXPECT_TRUE(irt_shm_os_is_owner(&owner));

    /* 第二个映射者: attach 已存在段, 非 owner */
    irt_shm_os_t user;
    std::memset(&user, 0, sizeof(user));
    void* base2 = irt_shm_os_map(&user, "/irt_test_osal", 4096);
    ASSERT_NE(base2, nullptr);
    EXPECT_FALSE(irt_shm_os_is_owner(&user));

    /* 写入 owner 视图, 从 user 视图可见 */
    static_cast<char*>(base)[0] = 0x5A;
    EXPECT_EQ(static_cast<char*>(base2)[0], 0x5A);

    irt_shm_os_unmap(&user);
    irt_shm_os_unmap(&owner);   /* owner unmap 触发 shm_unlink */
}
