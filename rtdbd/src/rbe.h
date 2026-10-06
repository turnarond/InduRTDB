/**
 * @file rbe.h
 * @brief rtdbd 服务端 RBE（Reporting By Exception）状态与决策（v3.7 主题A T1）
 *
 * RBE 状态存于 rtdbd 每连接上下文（非共享内存），换取零 ABI；
 * 代价是仅"经 rtdbd 写入"才享受 RBE 抑制。
 */
#ifndef RTDBD_RBE_H_
#define RTDBD_RBE_H_

#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>
#include <stdbool.h>
#include <stdint.h>

/* 每连接 RBE 槽上限 = 每连接订阅上限（定长，无堆分配） */
#define RTDBD_RBE_MAX RTDBD_SUB_MAX

typedef struct {
    uint32_t point_id;       /* 0 表示空槽 */
    double   last_reported;  /* 最后已上报值 */
    uint8_t  valid;          /* 0 = 尚未上报（首值必发） */
} rbe_slot_t;

typedef struct {
    rbe_slot_t slots[RTDBD_RBE_MAX];
} rbe_state_t;

void rbe_state_init(rbe_state_t* s);

/* 退订时清除某点位的 RBE 状态（保证重新订阅首值必发） */
void rbe_clear(rbe_state_t* s, uint32_t point_id);

/* RBE 决策：决定是否向该订阅者推送当前值。
 * 返回 true=推送（并更新 last_reported），false=抑制（死区未越过，基线不变）。
 * meta 提供 deadband/flags/eur 范围；value 为当前值(double)。 */
bool rbe_decide(rbe_state_t* s, uint32_t point_id, double value,
                const indurtdb_meta_t* meta);

#endif /* RTDBD_RBE_H_ */
