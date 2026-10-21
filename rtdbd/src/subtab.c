/**
 * @file subtab.c
 * @brief rtdbd 订阅成员 + RBE 状态合并哈希表（v3.10 任务 2）
 *
 * 开放寻址 + 线性探测；FNV-1a 对 4 个 id 字节求哈希。
 * 删除采用 backward-shift（Knuth Algorithm R）：无墓碑、无探测链断裂，
 * 也不需要 48KB 的临时重建数组或静态 scratch（对重入/多连接更安全）。
 */
#include "subtab.h"

#include <stddef.h>

#define RTDBD_SUBTAB_MASK (RTDBD_SUBTAB_CAP - 1u)

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

static uint32_t subtab_home(uint32_t point_id)
{
    return subtab_hash(point_id) & RTDBD_SUBTAB_MASK;
}

static rtdbd_subtab_slot_t* subtab_lookup(rtdbd_subtab_t* t, uint32_t point_id)
{
    uint32_t idx = subtab_home(point_id);
    for (uint32_t n = 0; n < RTDBD_SUBTAB_CAP; ++n) {
        rtdbd_subtab_slot_t* s = &t->slots[idx];
        if (!s->used) return NULL;           /* 空槽 = 探测链终点 */
        if (s->point_id == point_id) return s;
        idx = (idx + 1u) & RTDBD_SUBTAB_MASK;
    }
    return NULL;
}

void rtdbd_subtab_init(rtdbd_subtab_t* t)
{
    rtdbd_subtab_clear(t);
}

void rtdbd_subtab_clear(rtdbd_subtab_t* t)
{
    for (uint32_t i = 0; i < RTDBD_SUBTAB_CAP; ++i) {
        t->slots[i].used = 0;
        t->slots[i].valid = 0;
        t->slots[i].point_id = 0;
        t->slots[i].last_reported = 0.0;
    }
    t->count = 0;
}

bool rtdbd_subtab_has(const rtdbd_subtab_t* t, uint32_t point_id)
{
    uint32_t idx = subtab_home(point_id);
    for (uint32_t n = 0; n < RTDBD_SUBTAB_CAP; ++n) {
        const rtdbd_subtab_slot_t* s = &t->slots[idx];
        if (!s->used) return false;
        if (s->point_id == point_id) return true;
        idx = (idx + 1u) & RTDBD_SUBTAB_MASK;
    }
    return false;
}

bool rtdbd_subtab_insert(rtdbd_subtab_t* t, uint32_t point_id)
{
    uint32_t idx = subtab_home(point_id);
    for (uint32_t n = 0; n < RTDBD_SUBTAB_CAP; ++n) {
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
        idx = (idx + 1u) & RTDBD_SUBTAB_MASK;
    }
    return false; /* 表满 */
}

void rtdbd_subtab_remove(rtdbd_subtab_t* t, uint32_t point_id)
{
    uint32_t idx = subtab_home(point_id);
    uint32_t n;
    for (n = 0; n < RTDBD_SUBTAB_CAP; ++n) {
        if (!t->slots[idx].used) return; /* 不存在 */
        if (t->slots[idx].point_id == point_id) break;
        idx = (idx + 1u) & RTDBD_SUBTAB_MASK;
    }
    if (n == RTDBD_SUBTAB_CAP) return;

    /* 向后移位删除：把洞后面的探测链元素前移，直到遇到空槽。 */
    uint32_t hole = idx;
    uint32_t j = idx;
    for (;;) {
        j = (j + 1u) & RTDBD_SUBTAB_MASK;
        if (!t->slots[j].used) break;
        uint32_t k = subtab_home(t->slots[j].point_id);
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
