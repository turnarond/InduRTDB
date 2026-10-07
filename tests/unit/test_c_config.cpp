/**
 * @file test_c_config.cpp
 * @brief 配置加载器 + 配置校验单元测试
 *
 * 原有（v3.1.0）配置加载器用例：Defaults / LoadFileRoundTrip / LoadFileMissingUsesDefaults。
 *
 * v3.7 主题B B3 新增：indurtdb_validate_point_meta 的合法/非法全场景
 *   - 空指针 / 全零（合法）
 *   - EUR 开启但 eur_min >= eur_max（含相等）
 *   - deadband 为负
 *   - 百分比死区 > 100
 *   - flags 含未知位
 *   - 百分比死区但未启用量程
 *   - 组合合法场景
 * 并校验出错字段标识与错误原因文本。
 */
#include <gtest/gtest.h>
#include <cstdio>
#include <cstring>

#include <indurtdb/indurtdb.h>

extern "C" {
#include "core/irt_config.h"
}

/* ---- v3.1.0 原有：配置加载器 ---- */

TEST(CConfig, Defaults) {
    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    EXPECT_STREQ(cfg.instance_id, "default");
    EXPECT_EQ(cfg.max_points, 10000u);
    EXPECT_EQ(cfg.max_subscribers, 32u);
}

TEST(CConfig, LoadFileRoundTrip) {
    const char* path = "/tmp/irt_test_config.txt";
    FILE* f = fopen(path, "w");
    ASSERT_NE(f, nullptr);
    fprintf(f, "# InduRTDB config\n");
    fprintf(f, "instance_id=test_hvac\n");
    fprintf(f, "max_points=5000\n");
    fprintf(f, "max_subscribers=16\n");
    fprintf(f, "\n");  /* 空行 */
    fprintf(f, "# trailing comment\n");
    fclose(f);

    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    int rc = irt_config_load_file(&cfg, path);
    EXPECT_EQ(rc, 0);
    EXPECT_STREQ(cfg.instance_id, "test_hvac");
    EXPECT_EQ(cfg.max_points, 5000u);
    EXPECT_EQ(cfg.max_subscribers, 16u);
    remove(path);
}

TEST(CConfig, LoadFileMissingUsesDefaults) {
    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    int rc = irt_config_load_file(&cfg, "/tmp/irt_nonexistent_xyz.txt");
    EXPECT_EQ(rc, -1);
    /* 失败不改写已有值 */
    EXPECT_STREQ(cfg.instance_id, "default");
    EXPECT_EQ(cfg.max_points, 10000u);
}

/* ---- v3.7 主题B B3 新增：点位语义校验 ---- */

namespace {

indurtdb_meta_t make_meta()
{
    indurtdb_meta_t m;
    memset(&m, 0, sizeof(m));
    return m;
}

} // namespace

/* 空指针 → ERR_NULL */
TEST(ConfigValidate, NullMeta)
{
    uint32_t fld = 0xFFFF;
    EXPECT_EQ(indurtdb_validate_point_meta(nullptr, &fld), INDURTDB_CFG_ERR_NULL);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_NONE);
}

/* 全零 meta（flags=0，未启用任何语义）→ 合法 */
TEST(ConfigValidate, AllZeroIsValid)
{
    indurtdb_meta_t m = make_meta();
    uint32_t fld = 0xFFFF;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_OK);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_NONE);
}

/* EUR 开启且 eur_min < eur_max → 合法 */
TEST(ConfigValidate, EurOrderedIsValid)
{
    indurtdb_meta_t m = make_meta();
    m.flags   = INDURTDB_META_FLAG_EUR;
    m.eur_min = 0.0;
    m.eur_max = 100.0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, nullptr), INDURTDB_CFG_OK);
}

/* EUR 开启但 eur_min == eur_max → 非法（字段 EUR_MIN） */
TEST(ConfigValidate, EurEqualIsInvalid)
{
    indurtdb_meta_t m = make_meta();
    m.flags   = INDURTDB_META_FLAG_EUR;
    m.eur_min = 50.0;
    m.eur_max = 50.0;
    uint32_t fld = 0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_ERR_EUR_RANGE);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_EUR_MIN);
}

/* EUR 开启但 eur_min > eur_max → 非法（字段 EUR_MIN） */
TEST(ConfigValidate, EurReversedIsInvalid)
{
    indurtdb_meta_t m = make_meta();
    m.flags   = INDURTDB_META_FLAG_EUR;
    m.eur_min = 100.0;
    m.eur_max = 0.0;
    uint32_t fld = 0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_ERR_EUR_RANGE);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_EUR_MIN);
}

/* EUR 未启用时上下限乱序不报错（未启用即不检查） */
TEST(ConfigValidate, EurDisabledIgnoresRange)
{
    indurtdb_meta_t m = make_meta();
    m.flags   = 0;              /* EUR 未启用 */
    m.eur_min = 100.0;
    m.eur_max = 0.0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, nullptr), INDURTDB_CFG_OK);
}

/* deadband 为负 → 非法 */
TEST(ConfigValidate, NegativeDeadbandIsInvalid)
{
    indurtdb_meta_t m = make_meta();
    m.flags    = INDURTDB_META_FLAG_DEADBAND;
    m.deadband = -1.0f;
    uint32_t fld = 0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_ERR_DEADBAND);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_DEADBAND);
}

/* 百分比死区 > 100 → 非法 */
TEST(ConfigValidate, PercentDeadbandOver100IsInvalid)
{
    indurtdb_meta_t m = make_meta();
    m.flags    = INDURTDB_META_FLAG_EUR | INDURTDB_META_FLAG_DEADBAND |
                 INDURTDB_META_FLAG_DEADBAND_PCT;
    m.eur_min  = 0.0;
    m.eur_max  = 100.0;
    m.deadband = 150.0f;
    uint32_t fld = 0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_ERR_DEADBAND_PCT);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_DEADBAND);
}

/* 百分比死区 = 100 边界 → 合法 */
TEST(ConfigValidate, PercentDeadband100IsValid)
{
    indurtdb_meta_t m = make_meta();
    m.flags    = INDURTDB_META_FLAG_EUR | INDURTDB_META_FLAG_DEADBAND |
                 INDURTDB_META_FLAG_DEADBAND_PCT;
    m.eur_min  = 0.0;
    m.eur_max  = 100.0;
    m.deadband = 100.0f;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, nullptr), INDURTDB_CFG_OK);
}

/* flags 含未知位 → 非法 */
TEST(ConfigValidate, UnknownFlagsIsInvalid)
{
    indurtdb_meta_t m = make_meta();
    m.flags = (1u << 7);   /* 未知位 */
    uint32_t fld = 0;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, &fld), INDURTDB_CFG_ERR_FLAGS);
    EXPECT_EQ(fld, INDURTDB_CFG_FLD_FLAGS);
}

/* 百分比死区但未启用量程：合法（两者正交），仅由谓词提示静默失效 */
TEST(ConfigValidate, PercentWithoutEurIsValidButFlagged)
{
    indurtdb_meta_t m = make_meta();
    m.flags    = INDURTDB_META_FLAG_DEADBAND | INDURTDB_META_FLAG_DEADBAND_PCT;
    m.deadband = 10.0f;
    /* 未置 EUR 位，且 eur_min==eur_max==0（跨度无效）→ 校验放行 */
    EXPECT_EQ(indurtdb_validate_point_meta(&m, nullptr), INDURTDB_CFG_OK);
    /* 但谓词应提示：阈值退化为 0，死区永不触发 */
    EXPECT_TRUE(indurtdb_meta_pct_without_range(&m));

    /* 给出有效跨度后不再提示 */
    m.eur_min = 0.0;
    m.eur_max = 100.0;
    EXPECT_FALSE(indurtdb_meta_pct_without_range(&m));
}

/* 非百分比死区不触发该提示 */
TEST(ConfigValidate, PctHintOnlyAppliesToPercentMode)
{
    indurtdb_meta_t m = make_meta();
    m.flags = INDURTDB_META_FLAG_DEADBAND;   /* 绝对死区，无跨度概念 */
    m.deadband = 1.0f;
    EXPECT_FALSE(indurtdb_meta_pct_without_range(&m));
    EXPECT_FALSE(indurtdb_meta_pct_without_range(nullptr));
}

/* 绝对死区 + 合法量程 → 合法（不要求 EUR） */
TEST(ConfigValidate, AbsDeadbandWithEurIsValid)
{
    indurtdb_meta_t m = make_meta();
    m.flags    = INDURTDB_META_FLAG_EUR | INDURTDB_META_FLAG_DEADBAND;
    m.eur_min  = -10.0;
    m.eur_max  = 10.0;
    m.deadband = 0.5f;
    EXPECT_EQ(indurtdb_validate_point_meta(&m, nullptr), INDURTDB_CFG_OK);
}

/* 错误原因文本非空且互不相同（供日志打印） */
TEST(ConfigValidate, ErrorReasonsAreDistinct)
{
    EXPECT_STRNE(indurtdb_cfg_error_reason(INDURTDB_CFG_OK),
                 indurtdb_cfg_error_reason(INDURTDB_CFG_ERR_EUR_RANGE));
    EXPECT_STRNE(indurtdb_cfg_error_reason(INDURTDB_CFG_ERR_DEADBAND),
                 indurtdb_cfg_error_reason(INDURTDB_CFG_ERR_FLAGS));
    EXPECT_STRNE(indurtdb_cfg_error_reason(INDURTDB_CFG_ERR_TYPE), "");
}
