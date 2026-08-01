/**
 * @file test_c_multi_instance.cpp
 * @brief SRS §7.1 多实例隔离测试
 * @version 3.0.0
 */

#include <gtest/gtest.h>
#include <cstring>

extern "C" {
#include <indurtdb/indurtdb.h>
}

/* MI-01: 不同 instance_id 数据隔离 */
TEST(CMultiInstance, InstanceIsolation) {
    /* 实例 A */
    ASSERT_EQ(indurtdb_initialize("inst_A_x", 100, 4), 0);
    ASSERT_EQ(indurtdb_write_int32(5, 111), 0);
    EXPECT_EQ(indurtdb_get_write_count(), 1u);

    /* 验证 A */
    int32_t va;
    ASSERT_EQ(indurtdb_read_int32(5, &va), 0);
    EXPECT_EQ(va, 111);

    /* 实例 B 使用不同 instance_id — 但当前单例架构不允许同时双开 */
    /* 必须先 shut A 再开 B 来验证隔离 */
    indurtdb_shutdown();

    ASSERT_EQ(indurtdb_initialize("inst_B_x", 100, 4), 0);
    int32_t vb = -1;
    /* B 的 id=5 从未写入，读到的可能是 0(新段) 或 A 的残留(复用shm) */
    int ret = indurtdb_read_int32(5, &vb);
    /* 关键断言:不同 instance_id 的 shm 名称不同,完全隔离 */
    EXPECT_TRUE(ret != 0 || vb == 0)
        << "inst_B should not see inst_A data (shm names differ)";

    indurtdb_shutdown();
}

/* MI-02: 同 instance_id 不同进程共享, 已验证于集成测试 */
/* (此测试仅文档记录, 实际验证在 test_c_multi_process.cpp) */
TEST(CMultiInstance, DocumentationPlaceholder) {
    SUCCEED(); /* multi-process test already covers this */
}
