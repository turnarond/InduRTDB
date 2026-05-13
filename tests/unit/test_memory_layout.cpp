/**
 * @file test_memory_layout.cpp
 * @brief 内存布局测试
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include "indurtdb/types/memory_layout.hpp"

using namespace indurtdb;

TEST(MemoryLayoutTest, HeaderSize) {
    EXPECT_EQ(sizeof(InduRTDBHeader), 64);
    EXPECT_TRUE(std::is_pod_v<InduRTDBHeader>);
}

TEST(MemoryLayoutTest, HeaderAlignment) {
    InduRTDBHeader header;
    
    uintptr_t addr = reinterpret_cast<uintptr_t>(&header);
    EXPECT_EQ(addr % 64, 0);
    
    EXPECT_EQ(offsetof(InduRTDBHeader, magic), 0);
    EXPECT_EQ(offsetof(InduRTDBHeader, version), 4);
    EXPECT_EQ(offsetof(InduRTDBHeader, max_points), 8);
    EXPECT_EQ(offsetof(InduRTDBHeader, max_subscribers), 12);
    EXPECT_EQ(offsetof(InduRTDBHeader, write_seq), 16);
    EXPECT_EQ(offsetof(InduRTDBHeader, stats.writes), 24);
    EXPECT_EQ(offsetof(InduRTDBHeader, stats.timeouts), 32);
}

TEST(MemoryLayoutTest, PointDataSize) {
    EXPECT_EQ(sizeof(PointData), 128);
    EXPECT_TRUE(std::is_pod_v<PointData>);
}

TEST(MemoryLayoutTest, PointDataAlignment) {
    PointData point;
    
    uintptr_t addr = reinterpret_cast<uintptr_t>(&point);
    EXPECT_EQ(addr % 128, 0);
    
    EXPECT_EQ(offsetof(PointData, value), 0);
    EXPECT_EQ(offsetof(PointData, timestamp_ns), 32);
    EXPECT_EQ(offsetof(PointData, type), 40);
    EXPECT_EQ(offsetof(PointData, quality), 41);
    EXPECT_EQ(offsetof(PointData, unit), 42);
    EXPECT_EQ(offsetof(PointData, access), 44);
    EXPECT_EQ(offsetof(PointData, name), 45);
}

TEST(MemoryLayoutTest, PointDataValueUnion) {
    PointData point;
    
    point.value.b = true;
    EXPECT_EQ(point.value.b, true);
    
    point.value.i = 12345;
    EXPECT_EQ(point.value.i, 12345);
    
    point.value.d = 3.14159;
    EXPECT_DOUBLE_EQ(point.value.d, 3.14159);
    
    const char* test_str = "test";
    strncpy(point.value.str, test_str, sizeof(point.value.str) - 1);
    point.value.str[sizeof(point.value.str) - 1] = '\0';
    EXPECT_STREQ(point.value.str, test_str);
}

TEST(MemoryLayoutTest, PointDataStringSize) {
    PointData point;
    
    EXPECT_EQ(sizeof(point.value.str), 32);
    
    memset(point.value.str, 'A', sizeof(point.value.str));
    point.value.str[sizeof(point.value.str) - 1] = '\0';
    
    const char* long_str = "0123456789abcdef0123456789ABCDE";
    strncpy(point.value.str, long_str, sizeof(point.value.str) - 1);
    point.value.str[sizeof(point.value.str) - 1] = '\0';
    EXPECT_STREQ(point.value.str, long_str);
}

TEST(MemoryLayoutTest, PointDataNameSize) {
    PointData point;
    
    EXPECT_EQ(sizeof(point.name), 64);
    
    const char* test_name = "AHU_01.Supply_Temp";
    strncpy(point.name, test_name, sizeof(point.name) - 1);
    point.name[sizeof(point.name) - 1] = '\0';
    EXPECT_STREQ(point.name, test_name);
}

TEST(MemoryLayoutTest, SubscriberEntrySize) {
    EXPECT_EQ(sizeof(SubscriberEntry), 16);
    EXPECT_TRUE(std::is_pod_v<SubscriberEntry>);
}

TEST(MemoryLayoutTest, SubscriberEntryAlignment) {
    SubscriberEntry entry;
    
    uintptr_t addr = reinterpret_cast<uintptr_t>(&entry);
    EXPECT_EQ(addr % 16, 0);
    
    EXPECT_EQ(offsetof(SubscriberEntry, pid), 0);
    EXPECT_EQ(offsetof(SubscriberEntry, last_heartbeat_ns), 4);
}

TEST(MemoryLayoutTest, MemoryLayoutConsistency) {
    InduRTDBHeader header;
    PointData point;
    SubscriberEntry entry;
    
    EXPECT_TRUE(std::is_pod_v<InduRTDBHeader>);
    EXPECT_TRUE(std::is_pod_v<PointData>);
    EXPECT_TRUE(std::is_pod_v<SubscriberEntry>);
    
    EXPECT_EQ(sizeof(header), 64);
    EXPECT_EQ(sizeof(point), 128);
    EXPECT_EQ(sizeof(entry), 16);
}

TEST(MemoryLayoutTest, StaticAssertions) {
    EXPECT_TRUE(true);
}

