/**
 * @file test_config_loader.cpp
 * @brief ConfigLoader 单元测试 —— 轻量 YAML 点位配置解析
 * @version 2.1.0
 *
 * 通过写临时 YAML 文件验证解析: 字段映射、类型字符串、
 * 注释/空行容错、失败路径 (文件缺失/空配置)。
 */

#include <indurtdb/core/config_loader.hpp>
#include <gtest/gtest.h>
#include <cstdio>
#include <unistd.h>

namespace {

using indurtdb::core::ConfigResult;
using indurtdb::core::free_config_result;
using indurtdb::core::parse_point_config;
using indurtdb::core::parse_type_string;

class ConfigLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        snprintf(path_, sizeof(path_), "/tmp/indurtdb_cfg_%d.yaml", (int)getpid());
    }

    void TearDown() override { std::remove(path_); }

    bool write_yaml(const char* content) {
        FILE* f = std::fopen(path_, "w");
        if (!f) return false;
        std::fputs(content, f);
        std::fclose(f);
        return true;
    }

    char path_[128];
};

// 完整 YAML: 两个点位, 全部字段映射正确
TEST_F(ConfigLoaderTest, ParseValidYaml) {
    ASSERT_TRUE(write_yaml(
        "points:\n"
        "  - id: 1001\n"
        "    name: \"AHU_01.Supply_Temp\"\n"
        "    type: double\n"
        "    unit: 1\n"
        "    access: 1\n"
        "  - id: 1002\n"
        "    name: \"AHU_01.Fan_State\"\n"
        "    type: bool\n"
        "    unit: 0\n"
        "    access: 3\n"));

    ConfigResult r;
    ASSERT_TRUE(parse_point_config(path_, r));
    EXPECT_EQ(r.count, 2);

    EXPECT_EQ(r.points[0].id, 1001);
    EXPECT_STREQ(r.points[0].name, "AHU_01.Supply_Temp");
    EXPECT_EQ(r.points[0].type, 2);   // double
    EXPECT_EQ(r.points[0].unit, 1);
    EXPECT_EQ(r.points[0].access, 1);

    EXPECT_EQ(r.points[1].id, 1002);
    EXPECT_EQ(r.points[1].type, 0);   // bool
    EXPECT_EQ(r.points[1].access, 3);

    free_config_result(r);
}

// 注释与空行被忽略
TEST_F(ConfigLoaderTest, CommentsAndBlankLinesIgnored) {
    ASSERT_TRUE(write_yaml(
        "# 点位配置示例\n"
        "\n"
        "points:\n"
        "\n"
        "  - id: 7\n"
        "    name: \"X.Y\"\n"
        "    type: int32\n"
        "    unit: 0\n"
        "    access: 3\n"));

    ConfigResult r;
    ASSERT_TRUE(parse_point_config(path_, r));
    EXPECT_EQ(r.count, 1);
    EXPECT_EQ(r.points[0].id, 7);
    EXPECT_EQ(r.points[0].type, 1);   // int32

    free_config_result(r);
}

// 文件缺失 → 解析失败
TEST_F(ConfigLoaderTest, MissingFileReturnsFalse) {
    ConfigResult r;
    EXPECT_FALSE(parse_point_config("/nonexistent/indurtdb_cfg.yaml", r));
}

// 空配置 (无点位) → 解析失败, 且可安全释放
TEST_F(ConfigLoaderTest, EmptyPointsReturnsFalse) {
    ASSERT_TRUE(write_yaml("points:\n"));

    ConfigResult r;
    EXPECT_FALSE(parse_point_config(path_, r));
    free_config_result(r);   // 失败路径也可能分配了内存, 必须可安全释放
}

// 类型字符串映射: bool/int/int32/double/float/str/string
TEST(ConfigLoaderTypeTest, ParseTypeStrings) {
    EXPECT_EQ(parse_type_string("bool"), 0);
    EXPECT_EQ(parse_type_string("int"), 1);
    EXPECT_EQ(parse_type_string("int32"), 1);
    EXPECT_EQ(parse_type_string("double"), 2);
    EXPECT_EQ(parse_type_string("float"), 2);
    EXPECT_EQ(parse_type_string("str"), 3);
    EXPECT_EQ(parse_type_string("string"), 3);
    EXPECT_EQ(parse_type_string("unknown"), 0xff);
    EXPECT_EQ(parse_type_string(nullptr), 0xff);
}

} // namespace
