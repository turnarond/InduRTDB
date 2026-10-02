/**
 * @file test_c_version.cpp
 * @brief 版本宏一致性测试（须与 VERSION / CMake project(VERSION) 一致）
 *
 * 期望值由 CMake 注入（INDURTDB_EXPECT_VERSION_*），测试端不硬编码：
 * 这样"版本四处同步"由构建系统守护，发布时只改 VERSION / project(VERSION)
 * 与头文件宏三处，测试自动跟随，不会漂移。
 */
#include <indurtdb/indurtdb.h>
#include <gtest/gtest.h>

#ifndef INDURTDB_EXPECT_VERSION_MAJOR
#error "INDURTDB_EXPECT_VERSION_* 未由 CMake 注入，版本一致性无法守护"
#endif

namespace {
TEST(VersionTest, MacrosMatchExpected) {
    EXPECT_EQ(INDURTDB_VERSION_MAJOR, INDURTDB_EXPECT_VERSION_MAJOR);
    EXPECT_EQ(INDURTDB_VERSION_MINOR, INDURTDB_EXPECT_VERSION_MINOR);
    EXPECT_EQ(INDURTDB_VERSION_PATCH, INDURTDB_EXPECT_VERSION_PATCH);
    EXPECT_STREQ(INDURTDB_VERSION_STRING, INDURTDB_EXPECT_VERSION_STRING);
}
} // namespace
