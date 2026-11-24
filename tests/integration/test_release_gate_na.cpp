/**
 * @file test_release_gate_na.cpp
 * @brief 阶段一 4 个 Gate 冻结名 n/a 骨架 ctest（用户裁决）。
 * 目的：让 required_irs 13 名在 ctest 真实注册（堵"7名齐但T名未注册"漏洞），
 * 但明确声明"不产出达标证据、status=n/a、is_real_t2=false"——
 * 真达标由阶段二现场独立工具 + 独立证据工件提供（doc19 §5.5）。
 */
#include <gtest/gtest.h>

#include <string>

namespace {

static const char *kNaDeclaration =
    "required_irs registration placeholder; status=n/a; is_real_t2=false; "
    "no T1/T2/T3 pass evidence produced; real measurement via on-site independent tool (phase 2)";

// 断言 n/a 声明存在且明确（可证伪：删声明即 RED）。
static void expect_na_declaration(const char *gate_name)
{
    ASSERT_NE(gate_name, nullptr);
    const std::string decl(kNaDeclaration);
    EXPECT_NE(decl.find("status=n/a"), std::string::npos);
    EXPECT_NE(decl.find("is_real_t2=false"), std::string::npos);
    EXPECT_NE(decl.find("no T1/T2/T3 pass evidence"), std::string::npos);
    EXPECT_NE(decl.find("phase 2"), std::string::npos);
}

TEST(ReleaseGateNa, Gate_T1_SyncOverhead)
{
    expect_na_declaration("Gate_T1_SyncOverhead");
}

TEST(ReleaseGateNa, Gate_T2_LocalP99)
{
    expect_na_declaration("Gate_T2_LocalP99");
    // check_t1t2_bench.py:203 的 gate_label 是内部标签（非 ctest），
    // 不得被当作本骨架的达标证据；is_real_t2 必须为 false。
}

TEST(ReleaseGateNa, Gate_T3_WallClock)
{
    expect_na_declaration("Gate_T3_WallClock");
}

TEST(ReleaseGateNa, Gate_T3_RSS)
{
    expect_na_declaration("Gate_T3_RSS");
}

} // namespace
