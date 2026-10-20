/**
 * @file test_c_policy.cpp
 * @brief rtdbd 写权限策略引擎单测（v3.9 T3）
 *
 * 覆盖 T3 验收用例（策略文件格式改为**显式 4 段 + `*` 通配**）：
 *   - Policy.WildcardPartitionAllowed   `<uid>:*:<min>:<max>` 对任意分区放行
 *   - Policy.PartitionScopedAllowDeny   `<uid>:<partition>:<min>:<max>` 放行该分区、拒绝他分区
 *   - Policy.DenyByDefault              未匹配任何规则一律拒绝
 *   - Policy.MalformedLineRejected      非法行 fail-fast（带行号），不静默跳过
 * 另含边界：段数 / 数字 / 区间 / 上限 / 行超长 / 注释与空行 / 打不开文件 /
 * 分区 0 与通配不等价 / 原因文本互异。
 *
 * 本用例直接编译 `rtdbd/src/policy.c`（纯叶子模块，无外部依赖），
 * 见 `tests/unit/CMakeLists.txt` 的 test_c_policy 特化块。
 */
#include <gtest/gtest.h>

#include <cstdio>
#include <string>

#include "policy.h"

namespace {

std::string write_tmp(const std::string& body)
{
    std::string path = "/tmp/indurtdb_policy_unit.txt";
    FILE* f = fopen(path.c_str(), "w");
    if (f) {
        fwrite(body.data(), 1, body.size(), f);
        fclose(f);
    }
    return path;
}

int load_body(const std::string& body, irt_policy_t* p, uint32_t* line = nullptr)
{
    std::string path = write_tmp(body);
    return irt_policy_load(p, path.c_str(), line);
}

uint32_t kUid = 1000;

} // namespace

/* ---- T3 验收红用例 ---- */

TEST(Policy, WildcardPartitionAllowed)
{
    irt_policy_t p;
    ASSERT_EQ(load_body("1000:*:0:999\n", &p), IRT_POLICY_OK);
    ASSERT_EQ(p.count, 1u);
    EXPECT_EQ(p.rules[0].wildcard_partition, 1);

    /* 任意分区、区间内的 id 均放行 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 0), 1);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 999), 1);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 7, 500), 1);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 4294967295u, 500), 1);
    /* 区间外 / 其它 uid 拒绝 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 1000), 0);
    EXPECT_EQ(irt_policy_allows(&p, kUid + 1, 0, 500), 0);
}

TEST(Policy, PartitionScopedAllowDeny)
{
    irt_policy_t p;
    ASSERT_EQ(load_body("1000:2:100:200\n", &p), IRT_POLICY_OK);
    ASSERT_EQ(p.count, 1u);
    EXPECT_EQ(p.rules[0].wildcard_partition, 0);
    EXPECT_EQ(p.rules[0].partition, 2u);

    EXPECT_EQ(irt_policy_allows(&p, kUid, 2, 100), 1); /* 边界下 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 2, 200), 1); /* 边界上 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 2, 150), 1); /* 区间内 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 1, 150), 0); /* 他分区拒绝 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 3, 150), 0); /* 他分区拒绝 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 2, 99), 0);  /* 区间外拒绝 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 2, 201), 0);
}

TEST(Policy, DenyByDefault)
{
    irt_policy_t p;
    /* 空策略文件 → 0 条规则 → 全拒（= 不传 --policy） */
    ASSERT_EQ(load_body("", &p), IRT_POLICY_OK);
    EXPECT_EQ(p.count, 0u);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 0), 0);

    ASSERT_EQ(load_body("1000:*:0:10\n", &p), IRT_POLICY_OK);
    EXPECT_EQ(irt_policy_allows(&p, 2000, 0, 5), 0); /* uid 未命中 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 11), 0); /* id 越界 */
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 5), 1);  /* 命中 */
    /* NULL 策略一律拒绝 */
    EXPECT_EQ(irt_policy_allows(nullptr, kUid, 0, 5), 0);
}

TEST(Policy, MalformedLineRejected)
{
    /* 第 1 行注释、第 2 行合法、第 3 行空、第 4 行 5 段 → 报错须指向第 4 行 */
    const std::string body =
        "# comment line\n"
        "0:*:0:10\n"
        "\n"
        "1000:1:2:3:4\n";
    irt_policy_t p;
    uint32_t line = 0;
    EXPECT_EQ(load_body(body, &p, &line), IRT_POLICY_ERR_FIELDS);
    EXPECT_EQ(line, 4u);
}

/* ---- 格式迁移信号：旧 3 段写法不再是合法输入 ---- */

TEST(Policy, ThreeFieldNowRejected)
{
    irt_policy_t p;
    uint32_t line = 0;
    EXPECT_EQ(load_body("1000:0:999\n", &p, &line), IRT_POLICY_ERR_FIELDS);
    EXPECT_EQ(line, 1u);
}

/* ---- 边界 ---- */

TEST(Policy, PartitionZeroIsNotWildcard)
{
    irt_policy_t p;
    ASSERT_EQ(load_body("1000:0:0:63\n", &p), IRT_POLICY_OK);
    EXPECT_EQ(p.rules[0].wildcard_partition, 0);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 10), 1);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 1, 10), 0); /* 显式分区 0 ≠ 通配 */
}

TEST(Policy, BadNumberRejected)
{
    irt_policy_t p;
    uint32_t line = 0;
    EXPECT_EQ(load_body("1000:*:0:\n", &p, &line), IRT_POLICY_ERR_NUMBER);          /* 空段 */
    EXPECT_EQ(line, 1u);
    EXPECT_EQ(load_body("1000:*:0:abc\n", &p), IRT_POLICY_ERR_NUMBER);              /* 非数字 */
    EXPECT_EQ(load_body("1000:*:0:4294967296\n", &p), IRT_POLICY_ERR_NUMBER);       /* 溢出 uint32 */
    EXPECT_EQ(load_body("-1:*:0:10\n", &p), IRT_POLICY_ERR_NUMBER);                 /* 负号 */
    EXPECT_EQ(load_body("1000:x:0:10\n", &p), IRT_POLICY_ERR_NUMBER);               /* 第 2 段非数字非 * */
    EXPECT_EQ(load_body("1000:**:0:10\n", &p), IRT_POLICY_ERR_NUMBER);              /* ** */
    EXPECT_EQ(load_body("1000:*:0:4294967295\n", &p), IRT_POLICY_OK);               /* 上限合法 */
}

TEST(Policy, InvertedRangeRejected)
{
    irt_policy_t p;
    uint32_t line = 0;
    EXPECT_EQ(load_body("1000:*:10:9\n", &p, &line), IRT_POLICY_ERR_RANGE);
    EXPECT_EQ(line, 1u);
    EXPECT_EQ(load_body("1000:*:5:5\n", &p), IRT_POLICY_OK); /* 单点区间合法 */
}

TEST(Policy, TooManyRulesRejected)
{
    std::string body;
    for (int i = 0; i < IRT_POLICY_MAX_RULES + 1; ++i) body += "1000:*:0:10\n";

    irt_policy_t p;
    uint32_t line = 0;
    EXPECT_EQ(load_body(body, &p, &line), IRT_POLICY_ERR_TOOMANY);
    EXPECT_EQ(line, (uint32_t)IRT_POLICY_MAX_RULES + 1);
    EXPECT_EQ(p.count, (size_t)IRT_POLICY_MAX_RULES);
}

TEST(Policy, LineTooLongRuleRejected)
{
    irt_policy_t p;
    uint32_t line = 0;
    std::string body = "1000:*:0:" + std::string(300, '9') + "\n";
    EXPECT_EQ(load_body(body, &p, &line), IRT_POLICY_ERR_LINELEN);
    EXPECT_EQ(line, 1u);
}

TEST(Policy, LongCommentLineTolerated)
{
    /* 纯注释行可超长（中文注释 3 字节/字，不该把运维的注释头拖死） */
    irt_policy_t p;
    std::string body = "# " + std::string(300, 'x') + "\n1000:*:0:10\n";
    ASSERT_EQ(load_body(body, &p), IRT_POLICY_OK);
    EXPECT_EQ(p.count, 1u);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 5), 1);
}

TEST(Policy, CommentsAndFieldWhitespace)
{
    const std::string body =
        "# leading comment\n"
        "  \t\n"
        "  1000 : * : 0 : 63  # trailing comment\n"
        "1001:*:0:63\n";
    irt_policy_t p;
    ASSERT_EQ(load_body(body, &p), IRT_POLICY_OK);
    ASSERT_EQ(p.count, 2u);
    EXPECT_EQ(p.rules[0].wildcard_partition, 1);
    EXPECT_EQ(irt_policy_allows(&p, 1000, 3, 10), 1);
    EXPECT_EQ(irt_policy_allows(&p, 1001, 3, 10), 1);
}

TEST(Policy, OpenFailureAndArgs)
{
    irt_policy_t p;
    uint32_t line = 123;
    EXPECT_EQ(irt_policy_load(&p, "/tmp/indurtdb_no_such_policy_file.txt", &line),
              IRT_POLICY_ERR_OPEN);
    EXPECT_EQ(line, 0u); /* 非行定位类错误不写行号 */
    EXPECT_EQ(irt_policy_load(nullptr, "/tmp/x", &line), IRT_POLICY_ERR_ARG);
    EXPECT_EQ(irt_policy_load(&p, nullptr, &line), IRT_POLICY_ERR_ARG);
}

TEST(Policy, InitClearsState)
{
    irt_policy_t p;
    ASSERT_EQ(load_body("1000:*:0:10\n", &p), IRT_POLICY_OK);
    ASSERT_EQ(p.count, 1u);
    irt_policy_init(&p);
    EXPECT_EQ(p.count, 0u);
    EXPECT_EQ(irt_policy_allows(&p, kUid, 0, 5), 0);
    irt_policy_init(nullptr); /* 不得崩溃 */
}

/* 原因文本：所有结果码都有区分度且非空（供 rtdbd 日志打印） */
TEST(Policy, StrerrorDistinctAndNonEmpty)
{
    const int codes[] = {
        IRT_POLICY_OK,          IRT_POLICY_ERR_ARG,     IRT_POLICY_ERR_OPEN,
        IRT_POLICY_ERR_TOOMANY, IRT_POLICY_ERR_FIELDS,  IRT_POLICY_ERR_NUMBER,
        IRT_POLICY_ERR_RANGE,   IRT_POLICY_ERR_LINELEN,
    };
    const size_t n = sizeof(codes) / sizeof(codes[0]);
    for (size_t i = 0; i < n; ++i) {
        EXPECT_NE(irt_policy_strerror(codes[i]), nullptr);
        EXPECT_STRNE(irt_policy_strerror(codes[i]), "");
        for (size_t j = i + 1; j < n; ++j)
            EXPECT_STRNE(irt_policy_strerror(codes[i]),
                         irt_policy_strerror(codes[j]));
    }
}
