/**
 * @file test_c_candidate_set.cpp
 * @brief S3 有界去重候选集单测：重复键去重、容量截断、胜者不受影响。
 */
#include <gtest/gtest.h>

extern "C" {
#include "cluster/rtdbd_candidate_set.h"
#include "cluster/rtdbd_select_source.h"
}

#include <cstring>

namespace {

static rtdbd_source_candidate_t mk(uint64_t epoch, uint64_t off, const char *node)
{
    rtdbd_source_candidate_t c;
    std::memset(&c, 0, sizeof(c));
    c.epoch = epoch;
    c.offset = off;
    std::strncpy(c.node_id, node, sizeof(c.node_id) - 1u);
    c.valid = true;
    c.n_points = 1u;
    return c;
}

static bool add(rtdbd_candidate_set_t *s, uint64_t epoch, uint64_t off, const char *node)
{
    rtdbd_source_candidate_t c = mk(epoch, off, node);
    return rtdbd_candidate_set_add(s, &c);
}

} // namespace

TEST(RtdbdCandidateSet, DuplicateKeyOnlyOnce)
{
    rtdbd_candidate_set_t s;
    rtdbd_candidate_set_init(&s);
    EXPECT_TRUE(add(&s, 5, 1, "node-a"));
    EXPECT_FALSE(add(&s, 5, 1, "node-a")); // 重复
    EXPECT_EQ(s.duplicate_dropped, 1u);
    rtdbd_source_candidate_t out[8];
    uint32_t n = rtdbd_candidate_set_snapshot(&s, out, 8);
    EXPECT_EQ(n, 1u);
}

TEST(RtdbdCandidateSet, CapacityBoundedAndTruncated)
{
    rtdbd_candidate_set_t s;
    rtdbd_candidate_set_init(&s);
    /* 容量 64：注入 65 个不同键，第 65 个被拒。 */
    for (uint32_t i = 0; i < 64; ++i) {
        EXPECT_TRUE(add(&s, 1, i, "node-a"));
    }
    EXPECT_FALSE(add(&s, 1, 64, "node-a"));
    EXPECT_EQ(s.truncated, 1u);
    rtdbd_source_candidate_t out[64];
    EXPECT_EQ(rtdbd_candidate_set_snapshot(&s, out, 64), 64u);
}

TEST(RtdbdCandidateSet, DuplicatesDoNotChangeWinner)
{
    rtdbd_candidate_set_t s;
    rtdbd_candidate_set_init(&s);
    ASSERT_TRUE(add(&s, 10, 1, "node-a"));
    EXPECT_FALSE(add(&s, 10, 1, "node-a")); // 重复键被拒
    EXPECT_EQ(s.duplicate_dropped, 1u);
    ASSERT_TRUE(add(&s, 10, 2, "node-z"));
    rtdbd_source_candidate_t out[8];
    uint32_t n = rtdbd_candidate_set_snapshot(&s, out, 8);
    rtdbd_source_result_t r;
    ASSERT_TRUE(rtdbd_select_source(out, n, &r, "fnv1a64-v1:0000:0000:1"));
    EXPECT_STREQ(r.winner_node_id, "node-z");
    EXPECT_EQ(r.valid_candidate_count, 2u);
}
