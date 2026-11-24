/**
 * @file rtdbd_digest.c
 * @brief S1：FNV-1a-64 点集摘要实现（见 rtdbd_digest.h）。
 */
#include "rtdbd_digest.h"

#include <stdio.h>
#include <string.h>

#define FNV1A64_OFFSET 14695981039346656037ull
#define FNV1A64_PRIME  1099511628211ull

static void fnv1a64_bytes(uint64_t *h, const void *data, size_t len)
{
    const uint8_t *p = (const uint8_t *)data;
    for (size_t i = 0; i < len; ++i) {
        *h ^= (uint64_t)p[i];
        *h *= FNV1A64_PRIME;
    }
}

static void fnv1a64_u64(uint64_t *h, uint64_t v)
{
    fnv1a64_bytes(h, &v, sizeof(v));
}

/* 按 type 取 value union 的有效字节数（避免 32B union 尾部未初始化污染）。 */
static size_t value_effective_len(uint8_t type)
{
    switch (type) {
    case INDURTDB_TYPE_BOOL:    return sizeof(((indurtdb_point_t *)0)->value.b);
    case INDURTDB_TYPE_INT32:   return sizeof(((indurtdb_point_t *)0)->value.i);
    case INDURTDB_TYPE_UINT32:  return sizeof(((indurtdb_point_t *)0)->value.u32);
    case INDURTDB_TYPE_INT64:   return sizeof(((indurtdb_point_t *)0)->value.i64);
    case INDURTDB_TYPE_FLOAT:   return sizeof(((indurtdb_point_t *)0)->value.f);
    case INDURTDB_TYPE_DOUBLE:  return sizeof(((indurtdb_point_t *)0)->value.d);
    case INDURTDB_TYPE_STRING:  return sizeof(((indurtdb_point_t *)0)->value.str); /* 固定 32B */
    default:                    return 0u;
    }
}

bool rtdbd_digest_compute(const indurtdb_point_t *points, uint32_t count,
                          rtdbd_digest_t *out)
{
    if (!out || (!points && count > 0u)) return false;
    /* 每点先算 pair hash，再对所有点异或累积——异或交换律保证输入顺序无关
     * （13 号全等复制要求：同一逻辑点集无论本地枚举顺序都同摘要）。 */
    uint64_t sh = 0u;
    uint64_t vh = 0u;
    for (uint32_t i = 0; i < count; ++i) {
        const indurtdb_point_t *p = &points[i];
        uint64_t ps = FNV1A64_OFFSET;
        uint64_t pv = FNV1A64_OFFSET;
        /* 结构段：name 为稳定 key（id 不在点结构内），其余为点元数据。 */
        fnv1a64_bytes(&ps, p->name, sizeof(p->name));
        fnv1a64_u64(&ps, (uint64_t)p->partition);
        fnv1a64_u64(&ps, (uint64_t)p->type);
        fnv1a64_u64(&ps, (uint64_t)p->access);
        /* 值段：name + type + 有效字节 + quality + source_ts。 */
        fnv1a64_bytes(&pv, p->name, sizeof(p->name));
        fnv1a64_u64(&pv, (uint64_t)p->type);
        fnv1a64_bytes(&pv, &p->value, value_effective_len(p->type));
        fnv1a64_u64(&pv, (uint64_t)p->quality);
        fnv1a64_u64(&pv, p->source_timestamp_ns);
        sh ^= ps;
        vh ^= pv;
    }
    out->struct_hash = sh;
    out->value_digest = vh;
    out->n_points = count;
    snprintf(out->hex, sizeof(out->hex), "fnv1a64-v1:%016llx:%016llx:%u",
             (unsigned long long)sh, (unsigned long long)vh, (unsigned)count);
    return true;
}

bool rtdbd_digest_equal(const rtdbd_digest_t *a, const rtdbd_digest_t *b)
{
    if (!a || !b) return false;
    return a->struct_hash == b->struct_hash &&
           a->value_digest == b->value_digest &&
           a->n_points == b->n_points;
}
