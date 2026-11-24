/**
 * @file test_c_digest.cpp
 * @brief S1 digest 纯函数单测：确定性、按类型有效字节、算法版本标记。
 * 输入为点快照数组（不依赖 indurtdb_t 内部、不改 lib ABI）。
 */
#include <gtest/gtest.h>

extern "C" {
#include "cluster/rtdbd_digest.h"
}

#include <cstring>
#include <string>

namespace {

static void make_point(indurtdb_point_t *p, uint32_t id, const char *name,
                       uint8_t type, int64_t ival, double dval)
{
    std::memset(p, 0, sizeof(*p)); // 尾部清零：避免 union 未初始化污染
    p->name[0] = '\0'; // id 是遍历 key，非结构字段
    std::strncpy(p->name, name, sizeof(p->name) - 1u);
    p->type = type;
    p->quality = INDURTDB_QUALITY_GOOD;
    p->partition = 0u;
    p->source_timestamp_ns = 1000u + id;
    if (type == INDURTDB_TYPE_INT32 || type == INDURTDB_TYPE_INT64) {
        p->value.i64 = ival;
    } else if (type == INDURTDB_TYPE_DOUBLE || type == INDURTDB_TYPE_FLOAT) {
        p->value.d = dval;
    } else if (type == INDURTDB_TYPE_STRING) {
        std::strncpy(p->value.str, name, sizeof(p->value.str) - 1u);
    } else {
        p->value.b = (ival != 0);
    }
}

} // namespace

TEST(RtdbdDigest, DeterministicAcrossInputOrder)
{
    indurtdb_point_t p[3];
    make_point(&p[0], 1, "alpha", INDURTDB_TYPE_INT32, 10, 0);
    make_point(&p[1], 2, "beta", INDURTDB_TYPE_DOUBLE, 0, 3.14);
    make_point(&p[2], 3, "gamma", INDURTDB_TYPE_STRING, 0, 0);

    rtdbd_digest_t d1, d2;
    ASSERT_TRUE(rtdbd_digest_compute(p, 3, &d1));
    indurtdb_point_t rev[3] = {p[2], p[1], p[0]}; // 不同输入顺序
    ASSERT_TRUE(rtdbd_digest_compute(rev, 3, &d2));
    EXPECT_EQ(d1.struct_hash, d2.struct_hash);
    EXPECT_EQ(d1.value_digest, d2.value_digest);
    EXPECT_EQ(d1.n_points, 3u);
    EXPECT_EQ(std::string(d1.hex).find("fnv1a64-v1:"), 0u); // 算法版本前缀
}

TEST(RtdbdDigest, ValueChangeOnlyAffectsValueDigest)
{
    indurtdb_point_t p[1];
    make_point(&p[0], 1, "pt", INDURTDB_TYPE_INT32, 10, 0);
    rtdbd_digest_t d1, d2;
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d1));
    p[0].value.i64 = 99; // 只改值
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d2));
    EXPECT_EQ(d1.struct_hash, d2.struct_hash);
    EXPECT_NE(d1.value_digest, d2.value_digest);
}

TEST(RtdbdDigest, NameChangeAffectsStructHash)
{
    indurtdb_point_t p[1];
    make_point(&p[0], 1, "alpha", INDURTDB_TYPE_INT32, 10, 0);
    rtdbd_digest_t d1, d2;
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d1));
    make_point(&p[0], 1, "beta", INDURTDB_TYPE_INT32, 10, 0); // 只改名
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d2));
    EXPECT_NE(d1.struct_hash, d2.struct_hash);
}

TEST(RtdbdDigest, EmptySet)
{
    rtdbd_digest_t d;
    ASSERT_TRUE(rtdbd_digest_compute(nullptr, 0, &d));
    EXPECT_EQ(d.n_points, 0u);
}

TEST(RtdbdDigest, TypeSpecificBytesStable)
{
    // 同 type 同值，即使 union 尾部残留不同，也须同摘要（按 type 定有效字节）。
    indurtdb_point_t a, b;
    make_point(&a, 1, "i32", INDURTDB_TYPE_INT32, 42, 0);
    make_point(&b, 1, "i32", INDURTDB_TYPE_INT32, 42, 0);
    std::memset(reinterpret_cast<uint8_t *>(&b.value) + 4, 0xAA, 28); // 尾部污染
    rtdbd_digest_t da, db;
    ASSERT_TRUE(rtdbd_digest_compute(&a, 1, &da));
    ASSERT_TRUE(rtdbd_digest_compute(&b, 1, &db));
    EXPECT_EQ(da.value_digest, db.value_digest);
}

TEST(RtdbdDigest, EqualAndHex)
{
    indurtdb_point_t p[1];
    make_point(&p[0], 1, "pt", INDURTDB_TYPE_INT64, 7, 0);
    rtdbd_digest_t d1, d2;
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d1));
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d2));
    EXPECT_TRUE(rtdbd_digest_equal(&d1, &d2));
    p[0].value.i64 = 8;
    ASSERT_TRUE(rtdbd_digest_compute(p, 1, &d2));
    EXPECT_FALSE(rtdbd_digest_equal(&d1, &d2));
}
