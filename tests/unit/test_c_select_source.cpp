/**
 * @file test_c_select_source.cpp
 * @brief S3 逻辑选源单测：归一化 (epoch,node_id)、boot_id 禁入真值、
 *        两独立摘要源（非恒等）、P10 字段名隔离。
 */
#include <gtest/gtest.h>

extern "C" {
#include "cluster/rtdbd_epoch.h"
#include "cluster/rtdbd_select_source.h"
}

#include <cstring>
#include <string>

namespace {

static rtdbd_source_candidate_t cand(uint64_t epoch, const char *node_id,
                                     uint64_t boot_id, uint64_t sh,
                                     uint64_t vd, bool valid = true)
{
    rtdbd_source_candidate_t c;
    std::memset(&c, 0, sizeof(c));
    c.epoch = epoch;
    std::strncpy(c.node_id, node_id, sizeof(c.node_id) - 1u);
    c.boot_id = boot_id;
    c.struct_hash = sh;
    c.value_digest = vd;
    c.n_points = 3u;
    c.offset = 0u;
    c.valid = valid;
    return c;
}

} // namespace

TEST(RtdbdSelectSource, SameEpochLargerNodeIdWins)
{
    rtdbd_source_candidate_t cs[2] = {
        cand(10, "node-a", 100, 0xAA, 0x11),
        cand(10, "node-z", 200, 0xBB, 0x22),
    };
    rtdbd_source_result_t out;
    ASSERT_TRUE(rtdbd_select_source(cs, 2, &out, nullptr));
    EXPECT_EQ(out.winner_epoch, 10u);
    EXPECT_STREQ(out.winner_node_id, "node-z");
    EXPECT_EQ(out.valid_candidate_count, 2u);
}

TEST(RtdbdSelectSource, LargerEpochWinsRegardlessNodeId)
{
    rtdbd_source_candidate_t cs[2] = {
        cand(11, "node-a", 100, 0xAA, 0x11),
        cand(10, "node-z", 200, 0xBB, 0x22),
    };
    rtdbd_source_result_t out;
    ASSERT_TRUE(rtdbd_select_source(cs, 2, &out, nullptr));
    EXPECT_STREQ(out.winner_node_id, "node-a");
}

TEST(RtdbdSelectSource, BootIdDoesNotAffectWinner)
{
    rtdbd_source_candidate_t a[2] = {
        cand(10, "node-a", 999, 0xAA, 0x11),
        cand(10, "node-z", 1, 0xBB, 0x22),
    };
    rtdbd_source_candidate_t b[2] = {
        cand(10, "node-a", 1, 0xAA, 0x11),
        cand(10, "node-z", 999, 0xBB, 0x22),
    };
    rtdbd_source_result_t oa, ob;
    ASSERT_TRUE(rtdbd_select_source(a, 2, &oa, nullptr));
    ASSERT_TRUE(rtdbd_select_source(b, 2, &ob, nullptr));
    EXPECT_STREQ(oa.winner_node_id, ob.winner_node_id); // boot_id 反转不改变胜者
    EXPECT_EQ(oa.winner_epoch, ob.winner_epoch);
}

TEST(RtdbdSelectSource, InvalidCandidatesFiltered)
{
    rtdbd_source_candidate_t cs[2] = {
        cand(20, "node-x", 100, 0xAA, 0x11, false),
        cand(10, "node-y", 200, 0xBB, 0x22, true),
    };
    rtdbd_source_result_t out;
    ASSERT_TRUE(rtdbd_select_source(cs, 2, &out, nullptr));
    EXPECT_STREQ(out.winner_node_id, "node-y");
    EXPECT_EQ(out.valid_candidate_count, 1u);
}

TEST(RtdbdSelectSource, TwoIndependentDigestsNotTautology)
{
    rtdbd_source_candidate_t cs[1] = { cand(5, "node-a", 1, 0xAA, 0xBB) };
    rtdbd_source_result_t out;
    /* 本机摘要与胜者摘要来自不同输入：断言二者可不同（非恒等式）。 */
    ASSERT_TRUE(rtdbd_select_source(cs, 1, &out, "fnv1a64-v1:0000:0000:1"));
    EXPECT_STRNE(out.winner_digest_hex, out.selected_source_digest_hex);
    // 若调用方注入与胜者相同的摘要，二者才相等——证明字段独立、非代码恒等
    rtdbd_source_result_t out2;
    ASSERT_TRUE(rtdbd_select_source(cs, 1, &out2, out.winner_digest_hex));
    EXPECT_STREQ(out2.winner_digest_hex, out2.selected_source_digest_hex);
}

TEST(RtdbdSelectSource, JsonHasNoP10FieldNamesAndEvidenceKind)
{
    rtdbd_source_candidate_t cs[1] = { cand(5, "node-a", 1, 0xAA, 0xBB) };
    rtdbd_source_result_t out;
    ASSERT_TRUE(rtdbd_select_source(cs, 1, &out, "fnv1a64-v1:0000:0000:1"));
    char buf[1024];
    ASSERT_TRUE(rtdbd_select_source_json(&out, buf, sizeof(buf)));
    std::string j(buf);
    EXPECT_NE(j.find("\"evidence_kind\":\"logic-only\""), std::string::npos);
    EXPECT_EQ(j.find("nodes_converged"), std::string::npos);
    EXPECT_EQ(j.find("terminal_hash"), std::string::npos);
    EXPECT_EQ(j.find("dict_max_snapshot_match"), std::string::npos);
}
