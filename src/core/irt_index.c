/**
 * @file irt_index.c
 * @brief 共享内存内 name→id 索引实现 (开放寻址 + 墓碑)
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 */

#include "core/irt_index.h"
#include "internal/irt_seqlock.h"

#include <string.h>

/* seqlock 读重试上限: 索引查找非热路径, 冲突即重试, 不引入锁 */
#define IRT_INDEX_MAX_RETRY 16

uint32_t irt_hash_fnv1a32(const char* s)
{
    uint32_t h = 0x811C9DC5u;   /* FNV offset basis */
    if (!s) return h;
    const unsigned char* p = (const unsigned char*)s;
    for (; *p; ++p) {
        h ^= (uint32_t)(*p);
        h *= 0x01000193u;       /* FNV prime */
    }
    return h;
}

uint32_t irt_index_capacity_for(uint32_t max_points)
{
    return irt_layout_buckets(max_points);
}

int irt_index_bind(irt_index_t* ix, const irt_shm_t* s)
{
    if (!ix || !s || !s->base) return INDURTDB_ERR_ARG;
    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr || hdr->off_index == 0) return INDURTDB_ERR_ARG;

    ix->slots    = (irt_index_slot_t*)((char*)s->base + hdr->off_index);
    ix->capacity = irt_layout_buckets(s->max_points);
    ix->mask     = ix->capacity - 1u;
    return 0;
}

void irt_index_clear(irt_shm_t* s)
{
    irt_index_t ix;
    if (irt_index_bind(&ix, s) != 0) return;
    /* 全 0xFF => point_id = UINT32_MAX = EMPTY */
    memset(ix.slots, 0xFF, (size_t)ix.capacity * sizeof(irt_index_slot_t));

    irt_header_t* hdr = irt_shm_header(s);
    if (hdr) __atomic_store_n(&hdr->index_count, 0u, __ATOMIC_RELAXED);
}

/* v3.9 复合键哈希: 先混入 partition(小端 4B), 再混入 name(到 NUL 为止),
 * 末了做 2 步尾混(avalanche)。
 *
 * 尾混不可省略 —— 前置 4 个常量字节在 partition==0 时对 FNV-1a 等价于
 * 「只换 offset basis」: 状态每步 h ^= 0; h *= prime, 故 h0 -> h0*prime^4。
 * 那等于换了 FNV 的初始种子, 破坏了标准 basis 的扩散性质。实测(满载 512、
 * 桶数 1024、负载 0.5):
 *   纯 FNV1a32(name)              平均探测 0.246  最长 7   (v3.8 基线)
 *   partition‖name, 无尾混        平均探测 0.660  最长 17  (劣化 2.7x)
 *   partition‖name, 有尾混        平均探测 0.461  最长 8   (本实现)
 *   理想随机哈希基线              平均探测 0.465  最长 9
 * 即尾混后与理想随机哈希等效, 且分区跨越高位字节时仍充分扩散
 * (同名 "temp" 在 1024 个分区上映射到 645 个不同桶, 随机基线期望 647)。
 * 与 tools/irt_migrate/irt_migrate/layout.py:compose_hash 必须逐字节一致。 */
uint32_t irt_index_compose_hash(uint32_t partition, const char* name)
{
    uint32_t h = 0x811C9DC5u;   /* FNV offset basis */
    for (int i = 0; i < 4; ++i) {
        h ^= (uint32_t)((partition >> (8 * i)) & 0xFFu);
        h *= 0x01000193u;       /* FNV prime */
    }
    if (name) {
        const unsigned char* p = (const unsigned char*)name;
        for (; *p; ++p) {
            h ^= (uint32_t)(*p);
            h *= 0x01000193u;
        }
    }
    /* 尾混: 把已混入的高位分区信息扩散回桶索引所用的低位 */
    h ^= h >> 16;
    h *= 0x01000193u;
    h ^= h >> 13;
    h *= 0x01000193u;
    return h;
}

/* 冲突判定: hash 相等, 且槽指向点位的分区与名字同时匹配查询键。
 * 分区与 name 的权威副本都在点位区 (pts[pid]), 索引不重复存字符串。
 * 先比 hash 是快速否决 —— 分区或名字不同必然导致 hash 不同, 或在此被排除。 */
static inline bool irt_index_slot_matches(uint32_t slot_hash, uint32_t slot_pid,
                                          const indurtdb_point_t* pts,
                                          uint32_t hash, uint32_t partition,
                                          const char* name)
{
    if (slot_hash != hash) return false;
    if (!pts || slot_pid == IRT_INDEX_EMPTY || slot_pid == IRT_INDEX_TOMBSTONE)
        return false;
    if (pts[slot_pid].partition != partition) return false;
    return strncmp(pts[slot_pid].name, name,
                   sizeof(pts[slot_pid].name) - 1) == 0;
}

/* ---- 无锁变体: 调用方已持 seqlock 写锁 ---- */

int irt_index_insert_locked(irt_shm_t* s, uint32_t partition, const char* name,
                            uint32_t point_id)
{
    if (!s || !name || name[0] == '\0') return INDURTDB_ERR_ARG;

    irt_index_t ix;
    int rc = irt_index_bind(&ix, s);
    if (rc != 0) return rc;

    indurtdb_point_t* pts = irt_shm_points(s);
    if (!pts || point_id >= s->max_points) return INDURTDB_ERR_ARG;

    /* 索引键与点位属性一致性: 归属分区由本函数单点写入 */
    pts[point_id].partition = partition;

    const uint32_t h    = irt_index_compose_hash(partition, name);
    const uint32_t home = h & ix.mask;
    uint32_t       tomb = ix.capacity;      /* 首个墓碑槽, 可复用 */
    bool           placed = false;

    for (uint32_t d = 0; d < ix.capacity; ++d) {
        const uint32_t i   = (home + d) & ix.mask;
        const uint32_t pid = ix.slots[i].point_id;

        if (pid == IRT_INDEX_EMPTY) {
            const uint32_t dst = (tomb != ix.capacity) ? tomb : i;
            ix.slots[dst].hash     = h;
            ix.slots[dst].point_id = point_id;
            placed = true;
            break;
        }
        if (pid == IRT_INDEX_TOMBSTONE) {
            if (tomb == ix.capacity) tomb = i;   /* 记住首个墓碑, 继续探测确认不存在 */
            continue;
        }
        /* 已注册过同键(同分区同名): 更新为新 id (配置重载幂等) */
        if (irt_index_slot_matches(ix.slots[i].hash, pid, pts, h, partition, name)) {
            ix.slots[i].point_id = point_id;
            return 0;
        }
    }

    /* 全表无空槽但有墓碑: 复用墓碑 */
    if (!placed && tomb != ix.capacity) {
        ix.slots[tomb].hash     = h;
        ix.slots[tomb].point_id = point_id;
        placed = true;
    }
    if (!placed) return INDURTDB_ERR_FULL;

    irt_header_t* hdr = irt_shm_header(s);
    if (hdr) __atomic_add_fetch(&hdr->index_count, 1u, __ATOMIC_RELAXED);
    return 0;
}

int irt_index_remove_locked(irt_shm_t* s, uint32_t partition, const char* name)
{
    if (!s || !name || name[0] == '\0') return INDURTDB_ERR_ARG;

    irt_index_t ix;
    int rc = irt_index_bind(&ix, s);
    if (rc != 0) return rc;

    indurtdb_point_t* pts = irt_shm_points(s);
    if (!pts) return INDURTDB_ERR_ARG;

    const uint32_t h    = irt_index_compose_hash(partition, name);
    const uint32_t home = h & ix.mask;

    for (uint32_t d = 0; d < ix.capacity; ++d) {
        const uint32_t i   = (home + d) & ix.mask;
        const uint32_t pid = ix.slots[i].point_id;

        if (pid == IRT_INDEX_EMPTY) return INDURTDB_ERR_NOT_FOUND;  /* 探测链终止 */
        if (pid == IRT_INDEX_TOMBSTONE) continue;
        if (irt_index_slot_matches(ix.slots[i].hash, pid, pts, h, partition, name)) {
            /* 置墓碑而非清空: 否则会截断后续同桶项的探测链 */
            ix.slots[i].point_id = IRT_INDEX_TOMBSTONE;
            irt_header_t* hdr = irt_shm_header(s);
            if (hdr) {
                uint32_t cur = __atomic_load_n(&hdr->index_count, __ATOMIC_RELAXED);
                if (cur > 0) __atomic_sub_fetch(&hdr->index_count, 1u, __ATOMIC_RELAXED);
            }
            return 0;
        }
    }
    return INDURTDB_ERR_NOT_FOUND;
}

/* ---- 纯探测 (不带锁), 供 lookup 在读窗口内调用 ---- */
static int irt_index_probe(const irt_index_t* ix, const indurtdb_point_t* pts,
                           uint32_t partition, const char* name, uint32_t* out_id)
{
    const uint32_t h    = irt_index_compose_hash(partition, name);
    const uint32_t home = h & ix->mask;

    for (uint32_t d = 0; d < ix->capacity; ++d) {
        const uint32_t i   = (home + d) & ix->mask;
        const uint32_t pid = ix->slots[i].point_id;

        if (pid == IRT_INDEX_EMPTY) return INDURTDB_ERR_NOT_FOUND;
        if (pid == IRT_INDEX_TOMBSTONE) continue;
        if (irt_index_slot_matches(ix->slots[i].hash, pid, pts, h, partition, name)) {
            if (out_id) *out_id = pid;
            return 0;
        }
    }
    return INDURTDB_ERR_NOT_FOUND;
}

/* ---- 带锁变体 ---- */

int irt_index_insert(irt_shm_t* s, uint32_t partition, const char* name, uint32_t point_id)
{
    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr) return INDURTDB_ERR_ARG;

    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return INDURTDB_ERR_BUSY;

    int rc = irt_index_insert_locked(s, partition, name, point_id);
    irt_seqlock_write_end(&hdr->write_seq, seq0);
    return rc;
}

int irt_index_remove(irt_shm_t* s, uint32_t partition, const char* name)
{
    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr) return INDURTDB_ERR_ARG;

    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return INDURTDB_ERR_BUSY;

    int rc = irt_index_remove_locked(s, partition, name);
    irt_seqlock_write_end(&hdr->write_seq, seq0);
    return rc;
}

int irt_index_lookup(const irt_shm_t* s, uint32_t partition, const char* name, uint32_t* out_id)
{
    if (!s || !name || name[0] == '\0' || !out_id) return INDURTDB_ERR_ARG;

    irt_index_t ix;
    int rc = irt_index_bind(&ix, s);
    if (rc != 0) return rc;

    indurtdb_point_t* pts = irt_shm_points(s);
    if (!pts) return INDURTDB_ERR_ARG;

    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr) return INDURTDB_ERR_ARG;

    /* seqlock 读重试: 拷贝发生在校验窗口内, 窗口前后序列号一致才采信 */
    for (int attempt = 0; attempt < IRT_INDEX_MAX_RETRY; ++attempt) {
        uint64_t s0 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                 /* 写中, 稍后重试 */

        uint32_t id = 0;
        int      r  = irt_index_probe(&ix, pts, partition, name, &id);

        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint64_t s1 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
        if (s0 == s1) {
            if (r == 0) *out_id = id;
            return r;
        }
    }
    return INDURTDB_ERR_BUSY;
}
