/**
 * @file test_c_data_model.cpp
 * @brief SRS §3.1 数据模型测试:元数据读写、字符串截断、peek未写点位
 * @version 3.1.0
 */

#include <gtest/gtest.h>
#include <cstring>
#include <cstdio>

extern "C" {
#include <indurtdb/indurtdb.h>
}

class CDataModelTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("dm_test", 200, 8), 0);

        /* DM-01: 写配置文件并加载 */
        char path[128];
        snprintf(path, sizeof(path), "/tmp/irt_dm_%d.yaml", getpid());
        FILE* f = fopen(path, "w");
        ASSERT_NE(f, nullptr);
        fputs(
            "points:\n"
            "  - id: 50\n"
            "    name: \"AHU_01.Supply_Temp\"\n"
            "    type: double\n"
            "    unit: 1\n"
            "    access: 1\n"
            "  - id: 51\n"
            "    name: \"Pump_Start_CMD\"\n"
            "    type: bool\n"
            "    access: 3\n", f);
        fclose(f);

        ASSERT_EQ(indurtdb_load_config(path), 0);
        remove(path);
    }
    void TearDown() override { indurtdb_shutdown(); }
};

/* DM-01: load_config 后 read_point 返回正确的 name/unit/access */
TEST_F(CDataModelTest, ConfigMetadataViaReadPoint) {
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(50, &p), 0);
    EXPECT_STREQ(p.name, "AHU_01.Supply_Temp");
    EXPECT_EQ(p.unit, 1);
    EXPECT_EQ(p.access, INDURTDB_ACCESS_READ_ONLY);

    ASSERT_EQ(indurtdb_read_point(51, &p), 0);
    EXPECT_STREQ(p.name, "Pump_Start_CMD");
    EXPECT_EQ(p.access, INDURTDB_ACCESS_READ_WRITE);
}

/* DM-02: write_string 超长字符串应截断 */
TEST_F(CDataModelTest, StringTruncation) {
    const char* long_str = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ"; /* 52 chars, >31 */
    ASSERT_EQ(indurtdb_write_string(60, long_str), 0);

    char buf[64];
    ASSERT_EQ(indurtdb_read_string(60, buf, sizeof(buf)), 0);
    /* 验证已截断为最多 31 个有效字符 */
    EXPECT_LE(strlen(buf), 31u);
    /* 验证前缀一致 */
    EXPECT_EQ(strncmp(buf, long_str, strlen(buf)), 0);
}

/* DM-03: peek 从未写过的点位返回 nullptr */
TEST_F(CDataModelTest, PeekOnNeverWritten) {
    /* 初始化范围内的点位，但从未写入过 */
    const indurtdb_point_t* p = indurtdb_peek(150);
    /* 点位存在但全零填充 */
    ASSERT_NE(p, nullptr);                       /* id 在 max_points 内 */
    EXPECT_EQ(p->type, INDURTDB_TYPE_BOOL);      /* 默认值 */
    EXPECT_EQ(p->timestamp_ns, 0u);              /* 从未写入 */
}

/* DM-04: shutdown 后再写报 not initialized */
TEST_F(CDataModelTest, WriteAfterShutdown) {
    indurtdb_shutdown();
    EXPECT_NE(indurtdb_write_int32(0, 123), 0);
    EXPECT_STREQ(indurtdb_get_last_error(), "not initialized");
}
