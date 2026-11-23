/**
 * @file rtdbd_sub_snapshot.c
 * @brief h-A 订阅快照表实现（见 rtdbd_sub_snapshot.h）。
 *
 * 纯进程内模块：无 fd/socket/vsoa 依赖；单线程事件循环内使用。
 */
#include "cluster/rtdbd_sub_snapshot.h"

#include <rtdbd/protocol.h> /* RTDBD_SUB_MAX */
#include <stdlib.h>
#include <string.h>

typedef struct {
    bool     used;
    uint64_t sid;
    uint64_t tick;   /* LRU：越大越新 */
    rtdbd_sub_snap_identity_t ident;
    uint32_t *ids;   /* 受控分配，容量 n */
    uint32_t  n;
} snap_slot_t;

typedef struct {
    snap_slot_t slots[RTDBD_SUB_SNAP_MAX];
    uint64_t tick;
    rtdbd_sub_snapshot_stats_t stats;
} snap_table_t;

static snap_table_t g_snap;

static void snap_slot_reset(snap_slot_t *s)
{
    if (!s) return;
    free(s->ids);
    memset(s, 0, sizeof(*s));
}

static snap_slot_t *find_sid(uint64_t sid)
{
    for (uint32_t i = 0; i < RTDBD_SUB_SNAP_MAX; ++i) {
        if (g_snap.slots[i].used && g_snap.slots[i].sid == sid) {
            return &g_snap.slots[i];
        }
    }
    return NULL;
}

static bool identity_equal(const rtdbd_sub_snap_identity_t *a,
                           const rtdbd_sub_snap_identity_t *b)
{
    if (!a || !b) return false;
    if (a->uid != b->uid || a->partition != b->partition) return false;
    /* service_id 定长 64，hello/快照两端均 '\0' 截断；用字符串比较避免尾部垃圾。 */
    return strncmp(a->service_id, b->service_id, sizeof(a->service_id)) == 0;
}

bool rtdbd_sub_snapshot_save(uint64_t sid, const rtdbd_sub_snap_identity_t *ident,
                             const uint32_t *ids, uint32_t n)
{
    if (sid == 0u || !ident || n > RTDBD_SUB_MAX || (n > 0u && !ids)) {
        g_snap.stats.dropped++;
        return false;
    }

    snap_slot_t *slot = find_sid(sid);
    if (!slot) {
        /* 找空槽，否则 LRU 淘汰最旧 */
        uint32_t victim = RTDBD_SUB_SNAP_MAX;
        uint64_t min_tick = UINT64_MAX;
        for (uint32_t i = 0; i < RTDBD_SUB_SNAP_MAX; ++i) {
            if (!g_snap.slots[i].used) { victim = i; break; }
            if (g_snap.slots[i].tick < min_tick) {
                min_tick = g_snap.slots[i].tick;
                victim = i;
            }
        }
        if (victim >= RTDBD_SUB_SNAP_MAX) {
            g_snap.stats.dropped++;
            return false;
        }
        slot = &g_snap.slots[victim];
        if (slot->used) g_snap.stats.evicted++;
        snap_slot_reset(slot);
        slot->sid = sid;
        slot->used = true;
    }

    uint32_t *copy = NULL;
    if (n > 0u) {
        copy = (uint32_t *)malloc((size_t)n * sizeof(uint32_t));
        if (!copy) {
            g_snap.stats.dropped++;
            return false;
        }
        memcpy(copy, ids, (size_t)n * sizeof(uint32_t));
    }
    free(slot->ids);
    slot->ids = copy;
    slot->n = n;
    slot->ident = *ident;
    slot->ident.service_id[sizeof(slot->ident.service_id) - 1u] = '\0';
    slot->tick = ++g_snap.tick;
    g_snap.stats.saved++;
    return true;
}

bool rtdbd_sub_snapshot_load(uint64_t sid, const rtdbd_sub_snap_identity_t *ident,
                             uint32_t *out, uint32_t cap, uint32_t *out_n)
{
    snap_slot_t *slot = find_sid(sid);
    if (!slot) {
        g_snap.stats.missed++;
        return false;
    }
    if (!identity_equal(&slot->ident, ident)) {
        g_snap.stats.identity_mismatch++;
        return false;
    }
    if (slot->n > cap || (slot->n > 0u && !out)) {
        g_snap.stats.dropped++;
        return false;
    }
    if (slot->n > 0u) memcpy(out, slot->ids, (size_t)slot->n * sizeof(uint32_t));
    if (out_n) *out_n = slot->n;
    slot->tick = ++g_snap.tick; /* 命中刷新 LRU */
    g_snap.stats.loaded++;
    return true;
}

bool rtdbd_sub_snapshot_remove(uint64_t sid)
{
    snap_slot_t *slot = find_sid(sid);
    if (!slot) return false;
    snap_slot_reset(slot);
    return true;
}

void rtdbd_sub_snapshot_clear(void)
{
    for (uint32_t i = 0; i < RTDBD_SUB_SNAP_MAX; ++i) {
        snap_slot_reset(&g_snap.slots[i]);
    }
    memset(&g_snap.stats, 0, sizeof(g_snap.stats));
    g_snap.tick = 0u;
}

rtdbd_sub_snapshot_stats_t rtdbd_sub_snapshot_stats(void)
{
    return g_snap.stats;
}
