/**
 * @file rbe.c
 * @brief rtdbd 服务端 RBE（Reporting By Exception）状态与决策（v3.7 主题A T1）
 *
 * 纯逻辑模块，定长无堆：仅记录每连接已订阅点位的"最后已上报值"，
 * 供 notify_broadcast 在死区未越过时抑制变更通知（值仍已落库）。
 */
#include "rbe.h"

void rbe_state_init(rbe_state_t* s)
{
    for (unsigned int i = 0; i < RTDBD_RBE_MAX; ++i) {
        s->slots[i].point_id = 0;
        s->slots[i].last_reported = 0.0;
        s->slots[i].valid = 0;
    }
}

void rbe_clear(rbe_state_t* s, uint32_t point_id)
{
    for (unsigned int i = 0; i < RTDBD_RBE_MAX; ++i) {
        if (s->slots[i].point_id == point_id) {
            s->slots[i].point_id = 0;
            s->slots[i].valid = 0;
            return;
        }
    }
}

bool rbe_decide(rbe_state_t* s, uint32_t point_id, double value,
                const indurtdb_meta_t* meta)
{
    /* 定位该点 slot：命中优先；否则取首个空槽 */
    int idx = -1;
    for (unsigned int i = 0; i < RTDBD_RBE_MAX; ++i) {
        if (s->slots[i].point_id == point_id) { idx = i; break; }
        if (idx < 0 && s->slots[i].point_id == 0 && s->slots[i].valid == 0) idx = i;
    }
    if (idx < 0) return true; /* 槽满（不应发生，订阅上限一致）：兜底发送 */

    rbe_slot_t* slot = &s->slots[idx];

    if (!slot->valid) {
        slot->point_id = point_id;
        slot->last_reported = value;
        slot->valid = 1;
        return true; /* 首值必发 */
    }

    uint32_t flags = meta ? meta->flags : 0u;
    bool rbe_on = (flags & (INDURTDB_META_FLAG_DEADBAND | INDURTDB_META_FLAG_DEADBAND_PCT)) != 0;
    bool eur_on = (flags & INDURTDB_META_FLAG_EUR) != 0;

    /* RBE 与 EUR 均未启用 → 每次都发（等同现状，零行为变更） */
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
