/**
 * @file subtab.h
 * @brief rtdbd 订阅成员 + RBE 状态合并哈希表（v3.10 任务 2）
 *
 * 一张开放寻址表同时承担两件事：
 *   - 订阅成员判断（conn_has_sub → rtdbd_subtab_has，O(1)）；
 *   - 每点位 RBE 决策状态（last_reported / valid）。
 *
 * 定长槽、开放寻址；v3.11 起槽数组按需增长，连接关闭时释放。
 */
#ifndef RTDBD_SUBTAB_H_
#define RTDBD_SUBTAB_H_

#include <indurtdb/indurtdb.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_SUBTAB_MIN_CAP 2048u
#define RTDBD_SUBTAB_MAX_CAP 32768u

typedef struct {
    double   last_reported; /* 8B，先放以保持 16B/槽 */
    uint32_t point_id;
    uint8_t  valid;   /* 0 = first value not sent */
    uint8_t  used;    /* 0 = empty slot; supports point_id == 0 */
    uint16_t pad;
} rtdbd_subtab_slot_t;

#if defined(__cplusplus)
static_assert(sizeof(rtdbd_subtab_slot_t) == 16, "rtdbd_subtab_slot_t must be 16B");
#else
_Static_assert(sizeof(rtdbd_subtab_slot_t) == 16, "rtdbd_subtab_slot_t must be 16B");
#endif

typedef struct {
    rtdbd_subtab_slot_t* slots;
    uint32_t             cap;    /* 2 的幂 */
    uint32_t             count;
} rtdbd_subtab_t;

bool rtdbd_subtab_init(rtdbd_subtab_t* t, uint32_t initial_cap);
void rtdbd_subtab_free(rtdbd_subtab_t* t);
bool rtdbd_subtab_reserve(rtdbd_subtab_t* t, uint32_t min_cap);
void rtdbd_subtab_clear(rtdbd_subtab_t* t);
bool rtdbd_subtab_has(const rtdbd_subtab_t* t, uint32_t point_id);
bool rtdbd_subtab_insert(rtdbd_subtab_t* t, uint32_t point_id);
void rtdbd_subtab_remove(rtdbd_subtab_t* t, uint32_t point_id);
bool rtdbd_subtab_decide(rtdbd_subtab_t* t, uint32_t point_id, double value,
                         const indurtdb_meta_t* meta);

#ifdef __cplusplus
}
#endif

#endif /* RTDBD_SUBTAB_H_ */
