/**
 * @file test_basic_types.cpp
 * @brief 基础类型测试
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include "indurtdb/types/basic_types.hpp"

using namespace indurtdb;

TEST(BasicTypesTest, PointIdType) {
    PointId id1 = 1000;
    PointId id2 = 2000;
    
    EXPECT_EQ(sizeof(PointId), 4);
    EXPECT_LT(id1, id2);
}

TEST(BasicTypesTest, TimestampNsType) {
    TimestampNs ts1 = 1000000000ULL;
    TimestampNs ts2 = 2000000000ULL;
    
    EXPECT_EQ(sizeof(TimestampNs), 8);
    EXPECT_LT(ts1, ts2);
}

TEST(BasicTypesTest, PointTypeEnum) {
    EXPECT_EQ(static_cast<uint8_t>(PointType::BOOL), 0);
    EXPECT_EQ(static_cast<uint8_t>(PointType::INT32), 1);
    EXPECT_EQ(static_cast<uint8_t>(PointType::DOUBLE), 2);
    EXPECT_EQ(static_cast<uint8_t>(PointType::STRING), 3);
    
    EXPECT_LE(static_cast<uint8_t>(PointType::BOOL), 3);
    EXPECT_LE(static_cast<uint8_t>(PointType::STRING), 3);
}

TEST(BasicTypesTest, QualityEnum) {
    EXPECT_EQ(static_cast<uint8_t>(Quality::GOOD), 0);
    EXPECT_EQ(static_cast<uint8_t>(Quality::BAD), 1);
    EXPECT_EQ(static_cast<uint8_t>(Quality::TIMEOUT), 2);
    EXPECT_EQ(static_cast<uint8_t>(Quality::SUBSTITUTED), 3);
    
    EXPECT_LE(static_cast<uint8_t>(Quality::GOOD), 3);
    EXPECT_LE(static_cast<uint8_t>(Quality::SUBSTITUTED), 3);
}

TEST(BasicTypesTest, AccessEnum) {
    EXPECT_EQ(static_cast<uint8_t>(Access::READ_ONLY), 1);
    EXPECT_EQ(static_cast<uint8_t>(Access::READ_WRITE), 3);
    
    EXPECT_NE(static_cast<uint8_t>(Access::READ_ONLY), 
              static_cast<uint8_t>(Access::READ_WRITE));
}

TEST(BasicTypesTest, UnitEnum) {
    EXPECT_EQ(static_cast<uint16_t>(Unit::NO_UNIT), 0);
    EXPECT_EQ(static_cast<uint16_t>(Unit::DEGREES_CELSIUS), 1);
    EXPECT_EQ(static_cast<uint16_t>(Unit::PASCAL), 2);
    EXPECT_EQ(static_cast<uint16_t>(Unit::PERCENT), 3);
    
    EXPECT_EQ(sizeof(Unit), 2);
}

TEST(BasicTypesTest, TypeSizes) {
    EXPECT_EQ(sizeof(PointId), 4);
    EXPECT_EQ(sizeof(TimestampNs), 8);
    EXPECT_EQ(sizeof(PointType), 1);
    EXPECT_EQ(sizeof(Quality), 1);
    EXPECT_EQ(sizeof(Access), 1);
    EXPECT_EQ(sizeof(Unit), 2);
}

TEST(BasicTypesTest, EnumConversion) {
    PointType type = PointType::DOUBLE;
    Quality quality = Quality::GOOD;
    Access access = Access::READ_WRITE;
    Unit unit = Unit::DEGREES_CELSIUS;
    
    EXPECT_EQ(static_cast<uint8_t>(type), 2);
    EXPECT_EQ(static_cast<uint8_t>(quality), 0);
    EXPECT_EQ(static_cast<uint8_t>(access), 3);
    EXPECT_EQ(static_cast<uint16_t>(unit), 1);
}

