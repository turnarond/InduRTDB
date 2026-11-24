/**
 * @file rtdbd_epoch.h
 * @brief S1：进程级全局单调 epoch（13 号 D1/D2/D3、§8.3-1/:241/:246/:256）。
 *
 * epoch 是服务端分配的**全局纪元**（跨会话/节点可比），用于：
 * - 写记录携带 epoch，接收侧 epoch < current 拒绝（陈旧帧）；
 * - 终态裁决 (epoch, node_id) 字典序最大可达节点（13 号:256）。
 *
 * 与 rtdbd_session_t.epoch（会话级、reset 归零）**无关**——session 字段仅展示，
 * 不参与同步裁决。本模块不持久化；跨重启由 S2 /sync/hello 取 max 经
 * rtdbd_epoch_advance_to() 推进。boot_id 仅作 tie-break，不参与 IR6 真值判定。
 */
#ifndef RTDBD_EPOCH_H_
#define RTDBD_EPOCH_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    RTDBD_EPOCH_KEY_OK = 0,
    RTDBD_EPOCH_KEY_ARG = 1
} rtdbd_epoch_key_status_t;

/* 字典序裁决键：(epoch, node_id[32], boot_id) —— epoch 主序、node_id 次级、
 * boot_id 仅两级相等时的确定性 tie-break（24 号:87 boot_id 非唯一、不作真值判定）。 */
typedef struct {
    uint64_t epoch;
    char node_id[32];
    uint64_t boot_id;
} rtdbd_epoch_key_t;

/* 进程级全局 epoch 单例操作（单调递增、非零；线程内单线程调用）。 */
uint64_t rtdbd_epoch_next(void);           /* 分配下一个 epoch（> current） */
uint64_t rtdbd_epoch_current(void);        /* 当前 epoch（初始 0） */
bool rtdbd_epoch_is_stale(uint64_t frame_epoch); /* frame_epoch < current → true */
void rtdbd_epoch_advance_to(uint64_t epoch);     /* 推进到 max(current, epoch) */

/* 构造字典序键；比较返回 -1/0/1（a<b / a==b / a>b）。 */
rtdbd_epoch_key_status_t rtdbd_epoch_key(uint64_t epoch, uint64_t boot_id,
                                          const char *node_id, rtdbd_epoch_key_t *out);
int rtdbd_epoch_key_compare(const rtdbd_epoch_key_t *a, const rtdbd_epoch_key_t *b);

/* 仅供单测：重置全局状态（非生产 API）。 */
void rtdbd_epoch_reset_for_test(void);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_EPOCH_H_ */
