/**
 * @file test_version.cpp
 * @brief 版本号一致性单元测试
 * @version 2.1.0
 *
 * 版本宏必须与 CMakeLists.txt (project VERSION 2.1.0)、VERSION 文件保持一致,
 * 否则用户会在运行时看到错误的版本信息。
 */

#include <indurtdb.hpp>
#include <gtest/gtest.h>

namespace {

// 版本宏一致性:与 CMakeLists.txt / VERSION 文件对齐 (2.1.0)
TEST(VersionTest, MacroConsistency) {
    EXPECT_EQ(INDURTDB_VERSION_MAJOR, 2);
    EXPECT_EQ(INDURTDB_VERSION_MINOR, 1);
    EXPECT_EQ(INDURTDB_VERSION_PATCH, 0);
    EXPECT_STREQ(INDURTDB_VERSION_STRING, "2.1.0");
}

// 版本字符串必须与主头文件中的宏声明一致 (不能只改一个)
TEST(VersionTest, StringMatchesMacros) {
    char expected[32];
    snprintf(expected, sizeof(expected), "%d.%d.%d",
             INDURTDB_VERSION_MAJOR, INDURTDB_VERSION_MINOR, INDURTDB_VERSION_PATCH);
    EXPECT_STREQ(INDURTDB_VERSION_STRING, expected);
}

} // namespace
