/**
 * @file test_c_rbe.cpp
 * @brief v3.7 主题A：语义纯函数单测（值归一化 / EURange 量程位 / 死区判定）
 *
 * 纯函数、不触共享内存，无需 indurtdb_initialize。
 */
#include <indurtdb/indurtdb.h>

#include <gtest/gtest.h>

#include <cstdint>

/* ---- 值归一化：覆盖所有数值类型（含 int64 超 2^31） ---- */
TEST(SemanticsRBE, ValueToDoubleAllTypes) {
    bool b = true;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_BOOL, &b), 1.0);
    b = false;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_BOOL, &b), 0.0);

    int32_t  i  = -5;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_INT32, &i), -5.0);
    uint32_t u  = 4000000000u;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_UINT32, &u), 4000000000.0);
    int64_t  i64 = -9000000000LL;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_INT64, &i64), -9000000000.0);
    float    f  = 3.5f;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_FLOAT, &f), 3.5);
    double   d  = 2.25;
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_DOUBLE, &d), 2.25);

    /* 未知类型 → 0.0（不崩溃） */
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(INDURTDB_TYPE_STRING, nullptr), 0.0);
    EXPECT_DOUBLE_EQ(indurtdb_value_to_double(255, nullptr), 0.0);
}

/* ---- EURange 量程位 ---- */
TEST(SemanticsRBE, EurangeLimit) {
    EXPECT_EQ(indurtdb_eurange_limit(false, 0, 100, 50), INDURTDB_LIMIT_NONE); /* 未启用 */
    EXPECT_EQ(indurtdb_eurange_limit(true, 0, 100, 50), INDURTDB_LIMIT_NONE);  /* 区间内 */
    EXPECT_EQ(indurtdb_eurange_limit(true, 0, 100, -1), INDURTDB_LIMIT_LOW);   /* 越下界 */
    EXPECT_EQ(indurtdb_eurange_limit(true, 0, 100, 150), INDURTDB_LIMIT_HIGH); /* 越上界 */
    /* 边界含入（等于上下界不算越界） */
    EXPECT_EQ(indurtdb_eurange_limit(true, 0, 100, 0), INDURTDB_LIMIT_NONE);
    EXPECT_EQ(indurtdb_eurange_limit(true, 0, 100, 100), INDURTDB_LIMIT_NONE);
}

/* ---- 绝对死区：严格大于阈值才触发，等号不触发 ---- */
TEST(SemanticsRBE, DeadbandAbsolute) {
    EXPECT_TRUE(indurtdb_deadband_exceeded(true, 5.0, 0, 100, 0, 10));  /* 10>5 */
    EXPECT_FALSE(indurtdb_deadband_exceeded(true, 5.0, 0, 100, 0, 5));  /* 5 不 >5 */
    EXPECT_FALSE(indurtdb_deadband_exceeded(true, 5.0, 0, 100, 0, 4));
    /* 反向变化同样判定 */
    EXPECT_TRUE(indurtdb_deadband_exceeded(true, 5.0, 0, 100, 10, 0));
}

/* ---- 百分比死区：阈值 = deadband% * (eur_max - eur_min) ---- */
TEST(SemanticsRBE, DeadbandPercent) {
    /* span=100, 10% → 阈值 10 */
    EXPECT_TRUE(indurtdb_deadband_exceeded(false, 10.0, 0, 100, 0, 11));
    EXPECT_FALSE(indurtdb_deadband_exceeded(false, 10.0, 0, 100, 0, 10));
    EXPECT_FALSE(indurtdb_deadband_exceeded(false, 10.0, 0, 100, 0, 9));
    /* eur_max==eur_min：除零保护，不触发 */
    EXPECT_FALSE(indurtdb_deadband_exceeded(false, 10.0, 50, 50, 0, 1000));
}
