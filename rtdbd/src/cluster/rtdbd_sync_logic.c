/**
 * @file rtdbd_sync_logic.c
 * @brief S2 同步通道 cluster 层纯逻辑实现（见 rtdbd_sync_logic.h）。
 */
#include "rtdbd_sync_logic.h"

#include <limits.h>
#include <string.h>

uint32_t rtdbd_sync_delta_scan(indurtdb_t *h, uint32_t max_points,
                               uint64_t from_offset, uint32_t max_records, uint64_t epoch,
                               const char *node_id, uint64_t boot_id,
                               rtdbd_sync_delta_rec_t *recs, uint32_t cap,
                               uint64_t *applied, bool *has_more)
{
    if (!h || !recs || !applied || !has_more || !node_id) return 0u;
    uint32_t limit = max_records < cap ? max_records : cap;
    uint32_t n = 0u;
    uint64_t id = from_offset;
    *has_more = false;
    /* 上界必须是实际点容量（不能扫到 INT32_MAX：20 亿次 read_point 会卡死主循环）。 */
    uint64_t upper = from_offset > (uint64_t)max_points ? from_offset : (uint64_t)max_points;
    for (; id < upper; ++id) {
        indurtdb_point_t pt;
        if (indurtdb_h_read_point(h, (uint32_t)id, &pt) != 0) continue;
        if (pt.name[0] == '\0') continue; /* 未注册点 */
        if (n >= limit) { *has_more = true; break; }
        rtdbd_sync_delta_rec_t *r = &recs[n++];
        memset(r, 0, sizeof(*r));
        r->point_id = (uint32_t)id;
        memcpy(&r->value_bits, &pt.value, sizeof(r->value_bits));
        r->quality = pt.quality;
        r->type = pt.type;
        r->source_timestamp_ns = pt.source_timestamp_ns;
        r->epoch = epoch;
        strncpy(r->node_id, node_id, sizeof(r->node_id) - 1u);
        r->boot_id = boot_id;
        r->offset = id;
    }
    *applied = id; /* 最后扫描 id+1（未扫到的 id 已跳过，游标连续） */
    return n;
}

uint32_t rtdbd_sync_page_count(uint32_t n_points, uint32_t page_capacity)
{
    if (n_points == 0u || page_capacity == 0u) return 0u;
    return (n_points + page_capacity - 1u) / page_capacity;
}

void rtdbd_sync_wait_eval(const uint64_t *peer_epochs, uint32_t n_peers,
                          uint64_t min_epoch, uint32_t required_acks,
                          uint32_t *acks, bool *ok)
{
    uint32_t a = 0u;
    if (peer_epochs) {
        for (uint32_t i = 0; i < n_peers; ++i) {
            if (peer_epochs[i] >= min_epoch) ++a;
        }
    }
    if (acks) *acks = a;
    if (ok) *ok = (a >= required_acks);
}
