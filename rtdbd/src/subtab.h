/**
 * @file subtab.h
 * @brief rtdbd 订阅成员 + RBE 状态合并哈希表（v3.10 任务 2）
 *
 * 一张开放寻址表同时承担两件事：
 *   - 订阅成员判断（conn_has_sub → rtdbd_subtab_has，O(1)）；
 *   - 每点位 RBE 决策状态（last_reported / valid）。
 *
 * 定长、零堆分配；键为 point_id，point_id == 0 合法，空槽由 used 标记区分。
 */
#ifndef RTDBD_SUBTAB_H_
#define RTDBD_SUBTAB_H_

#include <indurtdb/indurtdb.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 2 * 每连接订阅上限 1024，向上取 2 的幂便于位掩码取模 */
#define RTDBD_SUBTAB_CAP 2048u

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
    rtdbd_subtab_slot_t slots[RTDBD_SUBTAB_CAP];
    uint32_t count;
} rtdbd_subtab_t;

void rtdbd_subtab_init(rtdbd_subtab_t* t);
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
