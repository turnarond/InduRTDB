/**
 * @file test_c_s5_capture.cpp
 * @brief S5 取证 JSON 构建/来源标注/校验纯函数单测（Linux 可测，板载同一 TU）。
 */
#include <gtest/gtest.h>

extern "C" {
#include "s5_capture.h"
}

#include <cstring>
#include <string>

namespace {

static s5_capture_t make_f2()
{
    s5_capture_t c;
    s5_capture_init(&c);
    strcpy(c.mode, "f2");
    strcpy(c.node_id, "192.168.30.220");
    s5_field_set_u64(&c, "epoch", 42, S5_SRC_ON_SITE);
    s5_field_set_u64(&c, "writes", 7, S5_SRC_ON_SITE);
    s5_field_set_u64(&c, "notify_latency_ns", 12345, S5_SRC_ON_SITE);
    /* P10 三字段 unavailable：null + reason */
    s5_field_set_unavailable(&c, "nodes_converged",
        "产品语义不足（S3不落shm/S2无跨机connector），现场不可得");
    s5_field_set_unavailable(&c, "terminal_hash",
        "产品语义不足（S3不落shm/S2无跨机connector），现场不可得");
    s5_field_set_unavailable(&c, "dict_max_snapshot_match",
        "产品语义不足（S3不落shm/S2无跨机connector），现场不可得");
    return c;
}

} // namespace

TEST(S5Capture, InitAndFieldSources)
{
    s5_capture_t c = make_f2();
    EXPECT_STREQ(c.mode, "f2");
    EXPECT_EQ(s5_field_source(&c, "epoch"), S5_SRC_ON_SITE);
    EXPECT_EQ(s5_field_source(&c, "nodes_converged"), S5_SRC_UNAVAILABLE);
    EXPECT_EQ(s5_field_source(&c, "no_such_field"), S5_SRC_NONE);
}

TEST(S5Capture, JsonContainsSourceTagsAndNullReason)
{
    s5_capture_t c = make_f2();
    char buf[4096];
    ASSERT_TRUE(s5_capture_emit_json(&c, buf, sizeof(buf)));
    std::string j(buf);
    EXPECT_NE(j.find("\"_source\":\"on-site\""), std::string::npos);
    EXPECT_NE(j.find("\"_source\":\"unavailable\""), std::string::npos);
    EXPECT_NE(j.find("\"nodes_converged\":{\"value\":null"), std::string::npos);
    EXPECT_NE(j.find("产品语义不足"), std::string::npos);
}

TEST(S5Capture, ValidateGood)
{
    s5_capture_t c = make_f2();
    EXPECT_TRUE(s5_capture_validate(&c));
}

TEST(S5Capture, UnavailableWithoutReasonFails)
{
    s5_capture_t c = make_f2();
    s5_field_set_unavailable(&c, "nodes_converged", ""); /* 无 reason */
    EXPECT_FALSE(s5_capture_validate(&c));
}

TEST(S5Capture, LogicOnlyCannotMapMeasured)
{
    s5_capture_t c = make_f2();
    s5_field_set_logic_only(&c, "winner_digest_hex", "fnv1a64-v1:aa:bb:3", "仅逻辑选源");
    /* 逻辑字段存在合法，但不得作为 P10 measured 字段：校验器须识别映射冲突 */
    s5_field_set_unavailable(&c, "terminal_hash", "不可得");
    EXPECT_TRUE(s5_capture_validate(&c)); /* 未冲突：logic-only 字段独立、未映射到 P10 */
    /* 冲突：把 logic-only 值写到 P10 字段（模拟谎标 on-site）→ FAIL */
    s5_field_set_on_site(&c, "terminal_hash", "fnv1a64-v1:aa:bb:3");
    EXPECT_FALSE(s5_capture_validate(&c)); /* P10 字段不得为 on-site（产品语义不足） */
}

TEST(S5Capture, F3aUnavailableFields)
{
    s5_capture_t c;
    s5_capture_init(&c);
    strcpy(c.mode, "f3a");
    s5_field_set_unavailable(&c, "partition_duration_s", "F3a注入工具未建、手段现场记录");
    s5_field_set_unavailable(&c, "stale_epoch_rejected", "无epoch注入能力、手段现场记录");
    EXPECT_TRUE(s5_capture_validate(&c));
    char buf[4096];
    ASSERT_TRUE(s5_capture_emit_json(&c, buf, sizeof(buf)));
    EXPECT_NE(std::string(buf).find("\"stale_epoch_rejected\":{\"value\":null"), std::string::npos);
}

TEST(S5Capture, FieldCountBounded)
{
    s5_capture_t c;
    s5_capture_init(&c);
    for (int i = 0; i < S5_CAPTURE_MAX_FIELDS; ++i) {
        char k[32];
        snprintf(k, sizeof(k), "f%d", i);
        EXPECT_TRUE(s5_field_set_u64(&c, k, (uint64_t)i, S5_SRC_ON_SITE));
    }
    EXPECT_FALSE(s5_field_set_u64(&c, "overflow", 1, S5_SRC_ON_SITE));
}

TEST(S5Capture, OversizeValueRejectedNotSilentlyTruncated)
{
    s5_capture_t c;
    s5_capture_init(&c);
    std::string huge(20000, 'x');
    char buf[8192];
    /* 超长值必须返回 false（报错），不得静默截断成假值。 */
    EXPECT_FALSE(s5_field_set_on_site(&c, "peers_summary", huge.c_str()));
    EXPECT_TRUE(s5_capture_emit_json(&c, buf, sizeof(buf)));
    EXPECT_EQ(std::string(buf).find("xxxx"), std::string::npos);
}

TEST(S5Capture, StatusFieldOnSite)
{
    s5_capture_t c;
    s5_capture_init(&c);
    ASSERT_TRUE(s5_field_set_status(&c, "deny_bad_token", 0x83u, S5_SRC_ON_SITE));
    char buf[8192];
    ASSERT_TRUE(s5_capture_emit_json(&c, buf, sizeof(buf)));
    EXPECT_NE(std::string(buf).find("\"deny_bad_token\":{\"value\":131"), std::string::npos);
}

TEST(S5Capture, F4CompareSameDigestAndPeers)
{
    s5_capture_t before, after;
    s5_capture_init(&before); s5_capture_init(&after);
    (void)s5_field_set_on_site(&before, "digest", "\"fnv1a64-v1:aa:bb:3\"");
    (void)s5_field_set_on_site(&after, "digest", "\"fnv1a64-v1:aa:bb:3\"");
    (void)s5_field_set_on_site(&before, "peers", "\"p1,p2\"");
    (void)s5_field_set_on_site(&after, "peers", "\"p1,p2\"");
    char b1[8192], b2[8192], out[8192];
    ASSERT_TRUE(s5_capture_emit_json(&before, b1, sizeof(b1)));
    ASSERT_TRUE(s5_capture_emit_json(&after, b2, sizeof(b2)));
    ASSERT_TRUE(s5_f4_compare(b1, b2, out, sizeof(out)));
    EXPECT_NE(std::string(out).find("\"digest_match\":true"), std::string::npos);
    EXPECT_NE(std::string(out).find("\"peers_restored\":true"), std::string::npos);
}

TEST(S5Capture, F4CompareDifferentDigest)
{
    s5_capture_t before, after;
    s5_capture_init(&before); s5_capture_init(&after);
    (void)s5_field_set_on_site(&before, "digest", "\"fnv1a64-v1:aa:bb:3\"");
    (void)s5_field_set_on_site(&after, "digest", "\"fnv1a64-v1:cc:dd:4\"");
    char b1[8192], b2[8192], out[8192];
    ASSERT_TRUE(s5_capture_emit_json(&before, b1, sizeof(b1)));
    ASSERT_TRUE(s5_capture_emit_json(&after, b2, sizeof(b2)));
    ASSERT_TRUE(s5_f4_compare(b1, b2, out, sizeof(out)));
    EXPECT_NE(std::string(out).find("\"digest_match\":false"), std::string::npos);
}

TEST(S5Capture, F4BothUnavailableDigestMustNotMatch)
{
    s5_capture_t before, after;
    s5_capture_init(&before); s5_capture_init(&after);
    (void)s5_field_set_unavailable(&before, "digest", "采集失败");
    (void)s5_field_set_unavailable(&after, "digest", "采集失败");
    char b1[8192], b2[8192], out[8192];
    ASSERT_TRUE(s5_capture_emit_json(&before, b1, sizeof(b1)));
    ASSERT_TRUE(s5_capture_emit_json(&after, b2, sizeof(b2)));
    ASSERT_TRUE(s5_f4_compare(b1, b2, out, sizeof(out)));
    /* 双侧 value=null 不得因字符串相等而假 true（S5c P0-1）。 */
    EXPECT_NE(std::string(out).find("\"digest_match\":false"), std::string::npos);
}

TEST(S5Capture, F4OneSideMissingDigestMustNotMatch)
{
    s5_capture_t before, after;
    s5_capture_init(&before); s5_capture_init(&after);
    (void)s5_field_set_on_site(&before, "digest", "\"fnv1a64-v1:aa:bb:3\"");
    char b1[8192], b2[8192], out[8192];
    ASSERT_TRUE(s5_capture_emit_json(&before, b1, sizeof(b1)));
    ASSERT_TRUE(s5_capture_emit_json(&after, b2, sizeof(b2)));
    ASSERT_TRUE(s5_f4_compare(b1, b2, out, sizeof(out)));
    EXPECT_NE(std::string(out).find("\"digest_match\":false"), std::string::npos);
}

TEST(S5Capture, F4EmptySetMarksWeakEvidence)
{
    const char *empty_hex = "\"fnv1a64-v1:0000000000000000:0000000000000000:0\"";
    s5_capture_t before, after;
    s5_capture_init(&before); s5_capture_init(&after);
    (void)s5_field_set_on_site(&before, "digest", empty_hex);
    (void)s5_field_set_on_site(&after, "digest", empty_hex);
    (void)s5_field_set_u64(&before, "digest_n_points", 0u, S5_SRC_ON_SITE);
    (void)s5_field_set_u64(&after, "digest_n_points", 0u, S5_SRC_ON_SITE);
    char b1[8192], b2[8192], out[8192];
    ASSERT_TRUE(s5_capture_emit_json(&before, b1, sizeof(b1)));
    ASSERT_TRUE(s5_capture_emit_json(&after, b2, sizeof(b2)));
    ASSERT_TRUE(s5_f4_compare(b1, b2, out, sizeof(out)));
    EXPECT_NE(std::string(out).find("\"digest_match\":true"), std::string::npos);
    EXPECT_NE(std::string(out).find("\"evidence_strength\":\"weak:empty-set\""), std::string::npos);
    EXPECT_NE(std::string(out).find("\"digest_n_points_before\":0"), std::string::npos);
    EXPECT_NE(std::string(out).find("\"digest_n_points_after\":0"), std::string::npos);
}
