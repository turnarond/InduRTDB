/**
 * @file test_c_version.cpp
 * @brief 版本宏一致性测试（须与 VERSION / CMake project(VERSION) 一致）
 * @version 3.1.0
 */
#include <indurtdb/indurtdb.h>
#include <gtest/gtest.h>

namespace {
TEST(VersionTest, MacrosMatchExpected) {
    EXPECT_EQ(INDURTDB_VERSION_MAJOR, 3);
    EXPECT_EQ(INDURTDB_VERSION_MINOR, 1);
    EXPECT_EQ(INDURTDB_VERSION_PATCH, 0);
    EXPECT_STREQ(INDURTDB_VERSION_STRING, "3.1.0");
}
} // namespace
