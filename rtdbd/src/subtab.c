/**
 * @file subtab.c
 * @brief rtdbd 订阅成员 + RBE 状态合并哈希表（v3.10 任务 2）
 *
 * 开放寻址 + 线性探测；FNV-1a 对 4 个 id 字节求哈希。
 * 删除采用 backward-shift（Knuth Algorithm R）：无墓碑、无探测链断裂。
 * v3.11 起槽数组按需增长（2048 → 32768），增长失败时旧表保持不变。
 */
#include "subtab.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

static uint32_t subtab_hash(uint32_t point_id)
{
    const uint8_t* p = (const uint8_t*)&point_id;
    uint32_t h = 2166136261u; /* FNV-1a offset basis */
    for (int i = 0; i < 4; ++i) {
        h ^= (uint32_t)p[i];
        h *= 16777619u;       /* FNV prime */
    }
    return h;
}

static uint32_t subtab_next_pow2(uint32_t v)
{
    uint32_t cap = RTDBD_SUBTAB_MIN_CAP;
    while (cap < v && cap < RTDBD_SUBTAB_MAX_CAP) cap <<= 1u;
    return cap;
}

static uint32_t subtab_home(const rtdbd_subtab_t* t, uint32_t point_id)
{
    return subtab_hash(point_id) & (t->cap - 1u);
}

static rtdbd_subtab_slot_t* subtab_lookup(rtdbd_subtab_t* t, uint32_t point_id)
{
    if (!t->slots || t->cap == 0) return NULL;
    uint32_t idx = subtab_home(t, point_id);
    for (uint32_t n = 0; n < t->cap; ++n) {
        rtdbd_subtab_slot_t* s = &t->slots[idx];
        if (!s->used) return NULL;
        if (s->point_id == point_id) return s;
        idx = (idx + 1u) & (t->cap - 1u);
    }
    return NULL;
}

bool rtdbd_subtab_reserve(rtdbd_subtab_t* t, uint32_t min_cap)
{
    if (!t) return false;
    if (min_cap <= t->cap) return true;

    uint32_t cap = subtab_next_pow2(min_cap);
    rtdbd_subtab_slot_t* slots = calloc(cap, sizeof(*slots));
    if (!slots) return false;

    for (uint32_t i = 0; i < t->cap; ++i) {
        if (!t->slots || !t->slots[i].used) continue;
        uint32_t idx = subtab_hash(t->slots[i].point_id) & (cap - 1u);
        while (slots[idx].used) idx = (idx + 1u) & (cap - 1u);
        slots[idx] = t->slots[i];
    }

    free(t->slots);
    t->slots = slots;
    t->cap = cap;
    return true;
}

bool rtdbd_subtab_init(rtdbd_subtab_t* t, uint32_t initial_cap)
{
    t->slots = NULL;
    t->cap = 0;
    t->count = 0;
    return rtdbd_subtab_reserve(t, initial_cap);
}

void rtdbd_subtab_free(rtdbd_subtab_t* t)
{
    free(t->slots);
    t->slots = NULL;
    t->cap = 0;
    t->count = 0;
}

void rtdbd_subtab_clear(rtdbd_subtab_t* t)
{
    if (t->slots && t->cap > 0) {
        memset(t->slots, 0, (size_t)t->cap * sizeof(*t->slots));
    }
    t->count = 0;
}

bool rtdbd_subtab_has(const rtdbd_subtab_t* t, uint32_t point_id)
{
    if (!t->slots || t->cap == 0) return false;
    uint32_t idx = subtab_home(t, point_id);
    for (uint32_t n = 0; n < t->cap; ++n) {
        const rtdbd_subtab_slot_t* s = &t->slots[idx];
        if (!s->used) return false;
        if (s->point_id == point_id) return true;
        idx = (idx + 1u) & (t->cap - 1u);
    }
    return false;
}

bool rtdbd_subtab_insert(rtdbd_subtab_t* t, uint32_t point_id)
{
    if (!t->slots && !rtdbd_subtab_init(t, RTDBD_SUBTAB_MIN_CAP)) return false;

    if ((t->count + 1u) > t->cap / 2u && t->cap < RTDBD_SUBTAB_MAX_CAP) {
        if (!rtdbd_subtab_reserve(t, t->cap * 2u)) return false;
    }

    uint32_t idx = subtab_home(t, point_id);
    for (uint32_t n = 0; n < t->cap; ++n) {
        rtdbd_subtab_slot_t* s = &t->slots[idx];
        if (!s->used) {
            s->point_id = point_id;
            s->last_reported = 0.0;
            s->valid = 0;
            s->pad = 0;
            s->used = 1;
            t->count++;
            return true;
        }
        if (s->point_id == point_id) return true; /* 已存在 */
        idx = (idx + 1u) & (t->cap - 1u);
    }
    return false; /* 表满 */
}

void rtdbd_subtab_remove(rtdbd_subtab_t* t, uint32_t point_id)
{
    if (!t->slots || t->cap == 0) return;

    uint32_t idx = subtab_home(t, point_id);
    uint32_t n;
    for (n = 0; n < t->cap; ++n) {
        if (!t->slots[idx].used) return;
        if (t->slots[idx].point_id == point_id) break;
        idx = (idx + 1u) & (t->cap - 1u);
    }
    if (n == t->cap) return;

    uint32_t hole = idx;
    uint32_t j = idx;
    for (;;) {
        j = (j + 1u) & (t->cap - 1u);
        if (!t->slots[j].used) break;
        uint32_t k = subtab_home(t, t->slots[j].point_id);
        bool in_range;
        if (hole <= j) in_range = (k > hole && k <= j);
        else           in_range = (k > hole || k <= j);
        if (!in_range) {
            t->slots[hole] = t->slots[j];
            hole = j;
        }
    }
    t->slots[hole].used = 0;
    t->slots[hole].valid = 0;
    t->count--;
}

bool rtdbd_subtab_decide(rtdbd_subtab_t* t, uint32_t point_id, double value,
                         const indurtdb_meta_t* meta)
{
    rtdbd_subtab_slot_t* slot = subtab_lookup(t, point_id);

    if (!slot) {
        /* 首次出现：插入并首值必发；表满兜底发送（不抑制）。 */
        if (!rtdbd_subtab_insert(t, point_id)) return true;
        slot = subtab_lookup(t, point_id);
        if (!slot) return true;
    }

    if (!slot->valid) {
        slot->last_reported = value;
        slot->valid = 1;
        return true; /* 首值必发 */
    }

    uint32_t flags = meta ? meta->flags : 0u;
    bool rbe_on = (flags & (INDURTDB_META_FLAG_DEADBAND | INDURTDB_META_FLAG_DEADBAND_PCT)) != 0;
    bool eur_on = (flags & INDURTDB_META_FLAG_EUR) != 0;

    /* RBE 与 EUR 均未启用 → 每次都发（与旧实现行为一致） */
    if (!rbe_on && !eur_on) {
        slot->last_reported = value;
        return true;
    }

    /* 死区越过？ */
    bool exceeded = false;
    if (rbe_on) {
        bool pct = (flags & INDURTDB_META_FLAG_DEADBAND_PCT) != 0;
        exceeded = indurtdb_deadband_exceeded(!pct, meta->deadband,
                                              meta->eur_min, meta->eur_max,
                                              slot->last_reported, value);
    }

    /* EURange 边界跨越？ */
    bool eur_cross = false;
    if (eur_on) {
        uint8_t lim_last = indurtdb_eurange_limit(true, meta->eur_min, meta->eur_max,
                                                 slot->last_reported);
        uint8_t lim_cur  = indurtdb_eurange_limit(true, meta->eur_min, meta->eur_max, value);
        eur_cross = (lim_last != lim_cur);
    }

    if (exceeded || eur_cross) {
        slot->last_reported = value;
        return true;
    }
    return false; /* 死区未越过且未跨量程边界：抑制 */
}
