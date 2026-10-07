/**
 * @file test_c_semantics.cpp
 * @brief T6：语义补齐测试（类型扩展 + 质量 OPC UA 映射 + 可用性判定）
 *
 * 覆盖：
 *  - 类型扩展 int64 / uint32 / float（复用 value union 32B，布局不变），v1 + v2 两条路径；
 *  - indurtdb_quality_to_status_code / indurtdb_status_code_to_quality 双向映射（含量程位）；
 *  - indurtdb_quality_is_usable（只看基础码）。
 */
#include <indurtdb/indurtdb.h>

#include <cstdint>
#include <cstring>

#include "gtest/gtest.h"

class CSemanticsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(indurtdb_initialize("sem_v1", 64, 4), 0);
    }
    void TearDown() override {
        indurtdb_shutdown();
    }
};

/* ---- 类型扩展：v1 路径 ---- */
TEST_F(CSemanticsTest, Int64RoundTrip) {
    ASSERT_EQ(indurtdb_write_int64(1, -9000000000LL), 0);
    int64_t out = 0;
    ASSERT_EQ(indurtdb_read_int64(1, &out), 0);
    EXPECT_EQ(out, -9000000000LL);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(1, &p), 0);
    EXPECT_EQ(p.type, INDURTDB_TYPE_INT64);
    EXPECT_EQ(p.value.i64, -9000000000LL);
}

TEST_F(CSemanticsTest, Uint32RoundTrip) {
    ASSERT_EQ(indurtdb_write_uint32(2, 4000000000u), 0);
    uint32_t out = 0;
    ASSERT_EQ(indurtdb_read_uint32(2, &out), 0);
    EXPECT_EQ(out, 4000000000u);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(2, &p), 0);
    EXPECT_EQ(p.type, INDURTDB_TYPE_UINT32);
}

TEST_F(CSemanticsTest, FloatRoundTrip) {
    ASSERT_EQ(indurtdb_write_float(3, 3.14159f), 0);
    float out = 0;
    ASSERT_EQ(indurtdb_read_float(3, &out), 0);
    EXPECT_EQ(out, 3.14159f);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(3, &p), 0);
    EXPECT_EQ(p.type, INDURTDB_TYPE_FLOAT);
}

TEST_F(CSemanticsTest, TypeTsPreservesSourceTimestamp) {
    ASSERT_EQ(indurtdb_write_int64_ts(4, 777, 9999), 0);
    indurtdb_point_t p;
    ASSERT_EQ(indurtdb_read_point(4, &p), 0);
    EXPECT_EQ(p.value.i64, 777);
    EXPECT_EQ(p.source_timestamp_ns, 9999u);
    EXPECT_EQ(p.quality, INDURTDB_QUALITY_GOOD);
}

/* ---- 类型扩展：v2 句柄路径 ---- */
TEST(SemanticsV2, FloatRoundTripViaHandle) {
    indurtdb_cfg_t cfg = {64, 4};
    indurtdb_t* h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, "sem_v2", &cfg), 0);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(indurtdb_h_write_float(h, 5, 2.5f), 0);
    float out = 0;
    EXPECT_EQ(indurtdb_h_read_float(h, 5, &out), 0);
    EXPECT_EQ(out, 2.5f);
    indurtdb_point_t p;
    EXPECT_EQ(indurtdb_h_read_point(h, 5, &p), 0);
    EXPECT_EQ(p.type, INDURTDB_TYPE_FLOAT);
    indurtdb_h_close(h);
}

TEST(SemanticsV2, Int64RoundTripViaHandle) {
    indurtdb_cfg_t cfg = {64, 4};
    indurtdb_t* h = nullptr;
    ASSERT_EQ(indurtdb_h_open(&h, "sem_v2b", &cfg), 0);
    ASSERT_NE(h, nullptr);
    EXPECT_EQ(indurtdb_h_write_int64(h, 6, 123456789012LL), 0);
    int64_t out = 0;
    EXPECT_EQ(indurtdb_h_read_int64(h, 6, &out), 0);
    EXPECT_EQ(out, 123456789012LL);
    indurtdb_h_close(h);
}

/* ---- OPC UA StatusCode 双向映射 ---- */
TEST(SemanticsStatusCode, RoundTripAllBaseAndLimit) {
    const uint8_t bases[] = {
        INDURTDB_QUALITY_GOOD, INDURTDB_QUALITY_BAD, INDURTDB_QUALITY_TIMEOUT,
        INDURTDB_QUALITY_SUBSTITUTED, INDURTDB_QUALITY_UNCERTAIN,
        INDURTDB_QUALITY_NOT_INITIALIZED, INDURTDB_QUALITY_OUT_OF_SERVICE,
        INDURTDB_QUALITY_COMM_FAILURE, INDURTDB_QUALITY_SENSOR_FAILURE,
        INDURTDB_QUALITY_LAST_KNOWN, INDURTDB_QUALITY_CONFIG_ERROR,
    };
    for (uint8_t b : bases) {
        for (uint8_t lim = 0; lim <= 3; lim++) {
            uint8_t q = INDURTDB_QUALITY_MAKE(b, lim);
            uint32_t sc = indurtdb_quality_to_status_code(q);
            uint8_t q2 = indurtdb_status_code_to_quality(sc);
            EXPECT_EQ(q2, q) << "base=" << (int)b << " limit=" << (int)lim;
        }
    }
}

TEST(SemanticsStatusCode, KnownConstants) {
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_GOOD), 0x00000000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_BAD), 0x80000000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_UNCERTAIN), 0x40000000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_SUBSTITUTED), 0x00D90000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_OUT_OF_SERVICE), 0x808D0000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_COMM_FAILURE), 0x80870000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_SENSOR_FAILURE), 0x808A0000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_CONFIG_ERROR), 0x80A10000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_LAST_KNOWN), 0x40900000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_NOT_INITIALIZED), 0x408D0000u);
    EXPECT_EQ(indurtdb_quality_to_status_code(INDURTDB_QUALITY_TIMEOUT), 0x408F0000u);
}

TEST(SemanticsStatusCode, UnknownStatusCodeFallsBackToBad) {
    /* 0x00000001: 无 limit 位、base 码不在映射表 -> 回退 BAD, limit 0 */
    uint8_t q = indurtdb_status_code_to_quality(0x00000001u);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(q), INDURTDB_QUALITY_BAD);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(q), 0u);
}

TEST(SemanticsStatusCode, LimitBitsSurviveInStatusCode) {
    uint8_t q = INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_GOOD, INDURTDB_LIMIT_HIGH);
    uint32_t sc = indurtdb_quality_to_status_code(q);
    uint8_t q2 = indurtdb_status_code_to_quality(sc);
    EXPECT_EQ(INDURTDB_QUALITY_BASE(q2), INDURTDB_QUALITY_GOOD);
    EXPECT_EQ(INDURTDB_QUALITY_LIMIT(q2), INDURTDB_LIMIT_HIGH);
}

/* ---- 可用性判定：只看基础码 ---- */
TEST(SemanticsQuality, IsUsableOnlyOnGoodBase) {
    EXPECT_TRUE(indurtdb_quality_is_usable(INDURTDB_QUALITY_GOOD));
    EXPECT_TRUE(indurtdb_quality_is_usable(
        INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_GOOD, INDURTDB_LIMIT_CONSTANT)));
    EXPECT_FALSE(indurtdb_quality_is_usable(INDURTDB_QUALITY_BAD));
    EXPECT_FALSE(indurtdb_quality_is_usable(INDURTDB_QUALITY_UNCERTAIN));
    EXPECT_FALSE(indurtdb_quality_is_usable(INDURTDB_QUALITY_OUT_OF_SERVICE));
    EXPECT_FALSE(indurtdb_quality_is_usable(INDURTDB_QUALITY_COMM_FAILURE));
}
