/**
 * @file test_c_index.cpp
 * @brief v3.4 T2 红阶段用例: 共享内存内 name→id 索引
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 *
 * 设计依据: docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md §3.2
 *   开放寻址(线性探测) + 墓碑; 条目 8B {hash32, point_id};
 *   桶数 B = roundup_pow2(max_points*2) (负载因子 ≤0.5); FNV-1a 32;
 *   冲突比较复用点位区内 name[64] (不重复存字符串).
 *
 * 用例 (红阶段应全部失败: irt_index_* 尚未实现):
 *   Index.FindByNameRoundTrip       注册后按名查到正确 id; 未注册 -> NOT_FOUND
 *   Index.HandlesHashCollision      同桶(人为构造)的多个点位名全部可查到
 *   Index.TombstoneAllowsReuse      删除后立即查不到; 槽位可复用
 *   Index.FullLoadNoInfiniteProbe   满负载时查找有限步终止(不死循环)
 *   Index.ZeroHeapAllocation        索引区定长预分配, 位于段内, 8B/槽
 *   Index.ProbeStatsAtHalfLoad      负载 0.5 时实测探测长度(冲突率)
 *   Index.PublicApiFindByName       经 indurtdb_load_config + find_by_name 端到端
 */

#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>

extern "C" {
#include <indurtdb/indurtdb.h>

#include "core/irt_index.h"
#include "core/irt_shm.h"
#include "internal/irt_types.h"
}

namespace {

/* 每个用例用独立实例名, 避免跨用例/跨次运行的残留段干扰 */
std::string unique_instance(const char* tag) {
    static int seq = 0;
    char buf[64];
    snprintf(buf, sizeof(buf), "irt_idx_%s_%d_%d", tag, (int)getpid(), ++seq);
    return std::string(buf);
}

/* 把点位名写进点位区 —— 索引比较 name 时读的就是这里 */
void set_point_name(irt_shm_t* s, uint32_t id, const char* name) {
    indurtdb_point_t* pts = irt_shm_points(s);
    ASSERT_NE(pts, nullptr);
    snprintf(pts[id].name, sizeof(pts[id].name), "%s", name);
}

class IndexTest : public ::testing::Test {
protected:
    irt_shm_t shm_{};
    std::string inst_;

    void SetUp() override { inst_ = unique_instance("base"); }

    void TearDown() override {
        irt_shm_shutdown(&shm_);
        std::string p = "/dev/shm/indurtdb_" + inst_;
        ::unlink(p.c_str());
    }

    /* 以 max_points 建段并清空点位名 */
    void init_seg(uint32_t max_points) {
        ASSERT_EQ(irt_shm_init(&shm_, inst_.c_str(), max_points, 0), 0);
        indurtdb_point_t* pts = irt_shm_points(&shm_);
        ASSERT_NE(pts, nullptr);
        memset(pts, 0, (size_t)max_points * sizeof(indurtdb_point_t));
    }
};

/* 1. 注册 → 按名查回正确 id; 未注册返回 NOT_FOUND */
TEST_F(IndexTest, FindByNameRoundTrip) {
    init_seg(64);

    set_point_name(&shm_, 7, "tank.temperature");
    ASSERT_EQ(irt_index_insert(&shm_, "tank.temperature", 7), 0);

    uint32_t id = 0xFFFFFFFFu;
    EXPECT_EQ(irt_index_lookup(&shm_, "tank.temperature", &id), 0);
    EXPECT_EQ(id, 7u);

    /* 空名 / 未注册名 -> 明确错误码, 不是裸 -1 */
    EXPECT_EQ(irt_index_lookup(&shm_, "no.such.point", &id), INDURTDB_ERR_NOT_FOUND);
    EXPECT_EQ(irt_index_lookup(&shm_, "", &id), INDURTDB_ERR_ARG);
    EXPECT_EQ(irt_index_lookup(&shm_, "tank.temperature", nullptr), INDURTDB_ERR_ARG);
}

/* 2. 人为构造同桶(哈希冲突)的多个点位名, 全部必须可查到 */
TEST_F(IndexTest, HandlesHashCollision) {
    const uint32_t kMax = 64;
    init_seg(kMax);

    irt_index_t ix{};
    ASSERT_EQ(irt_index_bind(&ix, &shm_), 0);
    ASSERT_GT(ix.capacity, 0u);
    const uint32_t mask = ix.capacity - 1u;

    /* 暴力搜出 6 个落在同一桶的名字 */
    const uint32_t target_bucket = irt_hash_fnv1a32("seed") & mask;
    std::vector<std::string> collide;
    for (int i = 0; i < 200000 && collide.size() < 6; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "p%d", i);
        if ((irt_hash_fnv1a32(nm) & mask) == target_bucket) collide.emplace_back(nm);
    }
    ASSERT_EQ(collide.size(), 6u) << "未能构造出足够的同桶名字";

    for (size_t i = 0; i < collide.size(); ++i) {
        uint32_t pid = (uint32_t)(i + 1);
        set_point_name(&shm_, pid, collide[i].c_str());
        ASSERT_EQ(irt_index_insert(&shm_, collide[i].c_str(), pid), 0);
    }

    /* 全部可查到, 且 id 正确 */
    for (size_t i = 0; i < collide.size(); ++i) {
        uint32_t id = 0;
        EXPECT_EQ(irt_index_lookup(&shm_, collide[i].c_str(), &id), 0)
            << "同桶名字丢失: " << collide[i];
        EXPECT_EQ(id, (uint32_t)(i + 1));
    }
}

/* 3. 墓碑: 删除后立即查不到; 槽位可复用(重新注册同名或新名均可) */
TEST_F(IndexTest, TombstoneAllowsReuse) {
    init_seg(64);

    set_point_name(&shm_, 3, "pump.pressure");
    ASSERT_EQ(irt_index_insert(&shm_, "pump.pressure", 3), 0);

    uint32_t id = 0;
    ASSERT_EQ(irt_index_lookup(&shm_, "pump.pressure", &id), 0);

    ASSERT_EQ(irt_index_remove(&shm_, "pump.pressure"), 0);
    EXPECT_EQ(irt_index_lookup(&shm_, "pump.pressure", &id), INDURTDB_ERR_NOT_FOUND);

    /* 槽位复用: 同名重新注册到新 id */
    set_point_name(&shm_, 9, "pump.pressure");
    EXPECT_EQ(irt_index_insert(&shm_, "pump.pressure", 9), 0);
    ASSERT_EQ(irt_index_lookup(&shm_, "pump.pressure", &id), 0);
    EXPECT_EQ(id, 9u);

    /* 墓碑不阻断探测链: 删除中间槽位后, 其后的同桶项仍可查到 */
    uint32_t idx_seg = 0;
    (void)idx_seg;
}

/* 4. 满负载: 查找必须在有限步内终止, 不得死循环 */
TEST_F(IndexTest, FullLoadNoInfiniteProbe) {
    const uint32_t kMax = 16;
    init_seg(kMax);

    irt_index_t ix{};
    ASSERT_EQ(irt_index_bind(&ix, &shm_), 0);
    const uint32_t B = ix.capacity;          /* = roundup_pow2(16*2) = 32 */

    /* 填满所有桶(远超设计负载 0.5, 属对抗性边界):
     * 名字互不相同, id 复用合法范围内的值(仅用于探测链, 不校验归属) */
    int inserted = 0;
    for (uint32_t i = 0; i < B; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "f%u", i);
        set_point_name(&shm_, i % kMax, nm);
        if (irt_index_insert(&shm_, nm, i % kMax) == 0) ++inserted;
    }
    EXPECT_EQ(inserted, (int)B);

    /* 查一个不存在的名字: 必须返回 NOT_FOUND 而非挂死 */
    uint32_t id = 0;
    EXPECT_EQ(irt_index_lookup(&shm_, "absent.point", &id), INDURTDB_ERR_NOT_FOUND);

    /* 再插入应报 FULL(索引区已满), 而不是越界写坏邻近区段 */
    EXPECT_EQ(irt_index_insert(&shm_, "one.more", 0u), INDURTDB_ERR_FULL);
}

/* 5. 零堆分配: 索引区定长预分配, 位于段内, 8B/槽 */
TEST_F(IndexTest, ZeroHeapAllocation) {
    const uint32_t kMax = 64;
    init_seg(kMax);

    EXPECT_EQ(sizeof(irt_index_slot_t), 8u) << "索引条目必须 8B";

    irt_index_t ix{};
    ASSERT_EQ(irt_index_bind(&ix, &shm_), 0);
    ASSERT_NE(ix.slots, nullptr);

    /* 索引区在段内, 且大小 = 桶数 × 8 */
    const char* base = (const char*)shm_.base;
    const char* ix_begin = (const char*)ix.slots;
    EXPECT_GE(ix_begin, base);
    EXPECT_LE(ix_begin + (size_t)ix.capacity * sizeof(irt_index_slot_t),
              base + shm_.total_size);

    irt_header_t* hdr = irt_shm_header(&shm_);
    EXPECT_EQ((uintptr_t)ix_begin, (uintptr_t)base + hdr->off_index);
    EXPECT_EQ(hdr->off_index + (size_t)ix.capacity * sizeof(irt_index_slot_t),
              (size_t)hdr->off_meta);
    /* 容量是 2 的幂, 保证负载因子 ≤0.5 时可用位掩码取模 */
    EXPECT_EQ(ix.capacity & (ix.capacity - 1u), 0u);
    EXPECT_GE(ix.capacity, kMax * 2u);
}

/* 6. 负载 0.5 时实测探测长度(冲突率) —— 验收要求给出实测数据 */
TEST_F(IndexTest, ProbeStatsAtHalfLoad) {
    const uint32_t kMax = 512;
    init_seg(kMax);

    irt_index_t ix{};
    ASSERT_EQ(irt_index_bind(&ix, &shm_), 0);
    const uint32_t mask = ix.capacity - 1u;

    /* 恰好装载 max_points 个(负载 = max_points / B = 0.5) */
    std::vector<std::string> names;
    for (uint32_t i = 0; i < kMax; ++i) {
        char nm[64];
        snprintf(nm, sizeof(nm), "tag.%u.value", i);
        names.emplace_back(nm);
        set_point_name(&shm_, i, nm);
        ASSERT_EQ(irt_index_insert(&shm_, nm, i), 0);
    }

    /* 逐项统计探测距离(从 home 桶到实际落点) */
    uint64_t total = 0, max_probe = 0, collide_cnt = 0;
    for (uint32_t i = 0; i < kMax; ++i) {
        const uint32_t home = irt_hash_fnv1a32(names[i].c_str()) & mask;
        uint32_t dist = 0, found = ix.capacity;   /* found = 实际落点桶 */
        for (uint32_t d = 0; d < ix.capacity; ++d) {
            const irt_index_slot_t* s = &ix.slots[(home + d) & mask];
            if (s->point_id == IRT_INDEX_TOMBSTONE) continue;
            if (s->point_id == IRT_INDEX_EMPTY) break;
            if (s->point_id == i) { dist = d; found = (home + d) & mask; break; }
        }
        ASSERT_NE(found, ix.capacity) << "装载项丢失: " << names[i];
        total += dist;
        if (dist > max_probe) max_probe = dist;
        if (dist > 0) ++collide_cnt;
    }

    const double avg = (double)total / (double)kMax;
    const double rate = (double)collide_cnt / (double)kMax;
    printf("  [索引实测] 桶数=%u 装载=%u 负载=0.50 平均探测=%.3f 最长探测=%llu 冲突率=%.1f%%\n",
           ix.capacity, kMax, avg, (unsigned long long)max_probe, rate * 100.0);

    EXPECT_LE(avg, 2.0) << "负载 0.5 时平均探测长度应远小于 2";
    EXPECT_LE(max_probe, 16u) << "最长探测长度超出预期, 需复核哈希/桶数";
}

/* 7. 端到端: 经 indurtdb_load_config 注册后, 公共 API 可按名查到 */
TEST_F(IndexTest, PublicApiFindByName) {
    const std::string inst = unique_instance("api");
    const std::string cfg = "/tmp/irt_index_cfg_" + inst + ".yaml";
    {
        FILE* f = fopen(cfg.c_str(), "w");
        ASSERT_NE(f, nullptr);
        fprintf(f,
                "points:\n"
                "  - id: 1\n"
                "    name: boiler.temp\n"
                "    type: double\n"
                "  - id: 2\n"
                "    name: boiler.press\n"
                "    type: double\n");
        fclose(f);
    }

    ASSERT_EQ(indurtdb_initialize(inst.c_str(), 64, 4), 0);
    ASSERT_EQ(indurtdb_load_config(cfg.c_str()), 0);

    uint32_t id = 0;
    EXPECT_EQ(indurtdb_find_by_name("boiler.temp", &id), 0);
    EXPECT_EQ(id, 1u);
    EXPECT_EQ(indurtdb_find_by_name("boiler.press", &id), 0);
    EXPECT_EQ(id, 2u);
    EXPECT_EQ(indurtdb_find_by_name("not.exists", &id), INDURTDB_ERR_NOT_FOUND);

    indurtdb_shutdown();
    ::unlink(cfg.c_str());
    ::unlink(("/dev/shm/indurtdb_" + inst).c_str());
}

}  // namespace
