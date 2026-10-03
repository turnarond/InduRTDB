/**
 * @file test_c_meta.cpp
 * @brief v3.4 T3 红阶段用例: 共享内存内元数据区 (每点 32B)
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 *
 * 设计依据: docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md §3.3
 *   每点 32B (冷数据, 按 point_id O(1) 索引):
 *     eur_min(8) eur_max(8) deadband(4) flags(4) reserved(8)
 *   元数据只存参数, 不上送执行, 不进入读写热路径.
 *
 * 用例 (红阶段应全部失败: 元数据区尚未启用、irt_meta_* 未实现):
 *   Meta.DefaultIsZero          未设置时 eur_min/max/deadband/flags 全 0
 *   Meta.SetGetRoundTrip        设置量程与死区后读回一致
 *   Meta.NotInHotPath           读写点位值不触碰元数据区 (字节级对照)
 *   Meta.OutOfRangeIdRejected   越界 id 返回明确错误码
 *   Meta.SizeIsThirtyTwoBytes   sizeof(indurtdb_meta_t) == 32 (ABI 锁死)
 *   Meta.PublicApiRoundTrip     indurtdb_set_meta / get_meta 端到端
 */

#include <gtest/gtest.h>

#include <cstring>
#include <string>

extern "C" {
#include <indurtdb/indurtdb.h>

#include "core/irt_shm.h"
#include "core/irt_point_manager.h"
#include "internal/irt_types.h"
}

namespace {

/* indurtdb_meta_t 32B 布局 (与 §3.3 对齐) */
IRT_STATIC_ASSERT(sizeof(indurtdb_meta_t) == 32, "meta struct must be 32 bytes");
IRT_STATIC_ASSERT(offsetof(indurtdb_meta_t, eur_min)   == 0,  "eur_min offset");
IRT_STATIC_ASSERT(offsetof(indurtdb_meta_t, eur_max)   == 8,  "eur_max offset");
IRT_STATIC_ASSERT(offsetof(indurtdb_meta_t, deadband)  == 16, "deadband offset");
IRT_STATIC_ASSERT(offsetof(indurtdb_meta_t, flags)     == 20, "flags offset");

std::string unique_instance(const char* tag) {
    static int seq = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "irt_meta_%s_%d_%d", tag, (int)getpid(), ++seq);
    return std::string(buf);
}

class MetaTest : public ::testing::Test {
protected:
    irt_shm_t shm_{};
    std::string inst_;

    void SetUp() override { inst_ = unique_instance("base"); }
    void TearDown() override {
        irt_shm_shutdown(&shm_);
        ::unlink(("/dev/shm/indurtdb_" + inst_).c_str());
    }
    void init_seg(uint32_t max_points) {
        ASSERT_EQ(irt_shm_init(&shm_, inst_.c_str(), max_points, 0), 0);
        indurtdb_point_t* pts = irt_shm_points(&shm_);
        ASSERT_NE(pts, nullptr);
        memset(pts, 0, (size_t)max_points * sizeof(indurtdb_point_t));
        irt_pm_t pm{};
        irt_pm_init(&pm, &shm_);
    }
};

/* 1. 未设置时全 0 */
TEST_F(MetaTest, DefaultIsZero) {
    init_seg(64);

    indurtdb_meta_t m{};
    EXPECT_EQ(irt_meta_get(&shm_, 0, &m), 0);
    EXPECT_DOUBLE_EQ(m.eur_min, 0.0);
    EXPECT_DOUBLE_EQ(m.eur_max, 0.0);
    EXPECT_FLOAT_EQ(m.deadband, 0.0f);
    EXPECT_EQ(m.flags, 0u);
}

/* 2. 设置 -> 读回一致 */
TEST_F(MetaTest, SetGetRoundTrip) {
    init_seg(64);

    indurtdb_meta_t in{};
    in.eur_min  = -10.0;
    in.eur_max  = 120.0;
    in.deadband = 0.5f;
    in.flags    = 0x3u;     /* 量程 + 死区启用 */

    ASSERT_EQ(irt_meta_set(&shm_, 5, &in), 0);

    indurtdb_meta_t out{};
    ASSERT_EQ(irt_meta_get(&shm_, 5, &out), 0);
    EXPECT_DOUBLE_EQ(out.eur_min, -10.0);
    EXPECT_DOUBLE_EQ(out.eur_max, 120.0);
    EXPECT_FLOAT_EQ(out.deadband, 0.5f);
    EXPECT_EQ(out.flags, 0x3u);
}

/* 3. 读写点位值 (热路径) 不触碰元数据区 */
TEST_F(MetaTest, NotInHotPath) {
    const uint32_t kMax = 64;
    init_seg(kMax);

    /* 把整片元数据区填成探针值 0xA5, 之后比对是否被破坏 */
    indurtdb_meta_t* meta = irt_shm_meta(&shm_);
    ASSERT_NE(meta, nullptr);
    unsigned char* raw = (unsigned char*)meta;
    size_t meta_bytes = (size_t)kMax * sizeof(indurtdb_meta_t);
    memset(raw, 0xA5, meta_bytes);
    __atomic_thread_fence(__ATOMIC_RELEASE);

    irt_pm_t pm{};
    irt_pm_init(&pm, &shm_);

    /* 反复写 + 读点位值 (热路径) */
    for (int i = 0; i < 1000; ++i) {
        ASSERT_EQ(irt_pm_write_double(&pm, 3, (double)i), 0);
        indurtdb_point_t p{};
        ASSERT_EQ(irt_pm_read(&pm, 7, &p), 0);
        (void)p;
    }

    /* 元数据区应原封不动 */
    bool intact = true;
    for (size_t i = 0; i < meta_bytes; ++i) {
        if (raw[i] != 0xA5) { intact = false; break; }
    }
    EXPECT_TRUE(intact) << "热路径读写点位值改动了元数据区";
}

/* 4. 越界 id 返回明确错误码 */
TEST_F(MetaTest, OutOfRangeIdRejected) {
    init_seg(64);

    indurtdb_meta_t m{};
    EXPECT_EQ(irt_meta_get(&shm_, 64, &m), INDURTDB_ERR_ARG);   /* id == max_points */
    EXPECT_EQ(irt_meta_get(&shm_, 0xFFFFFFFFu, &m), INDURTDB_ERR_ARG);
    EXPECT_EQ(irt_meta_set(&shm_, 64, &m), INDURTDB_ERR_ARG);
    EXPECT_EQ(irt_meta_set(&shm_, 0, nullptr), INDURTDB_ERR_ARG);
    EXPECT_EQ(irt_meta_get(&shm_, 0, nullptr), INDURTDB_ERR_ARG);
}

/* 5. ABI: 元数据条目 32B */
TEST_F(MetaTest, SizeIsThirtyTwoBytes) {
    EXPECT_EQ(sizeof(indurtdb_meta_t), 32u);
}

/* 6. 公共 API 端到端 */
TEST_F(MetaTest, PublicApiRoundTrip) {
    const std::string inst = unique_instance("api");
    ASSERT_EQ(indurtdb_initialize(inst.c_str(), 64, 4), 0);

    indurtdb_meta_t in{};
    in.eur_min  = 0.0;
    in.eur_max  = 100.0;
    in.deadband = 1.0f;
    in.flags    = 0x1u;
    ASSERT_EQ(indurtdb_set_meta(10, &in), 0);

    indurtdb_meta_t out{};
    ASSERT_EQ(indurtdb_get_meta(10, &out), 0);
    EXPECT_DOUBLE_EQ(out.eur_min, 0.0);
    EXPECT_DOUBLE_EQ(out.eur_max, 100.0);
    EXPECT_FLOAT_EQ(out.deadband, 1.0f);
    EXPECT_EQ(out.flags, 0x1u);

    EXPECT_EQ(indurtdb_get_meta(64, &out), INDURTDB_ERR_ARG);

    indurtdb_shutdown();
    ::unlink(("/dev/shm/indurtdb_" + inst).c_str());
}

}  // namespace
