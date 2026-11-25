/**
 * @file rtdbd_candidate_set.h
 * @brief S3 有界去重候选集（纯逻辑，不写 shm）。
 * 键 (epoch, offset, node_id)；定长 64；重复/截断计数可证伪。
 */
#ifndef RTDBD_CANDIDATE_SET_H_
#define RTDBD_CANDIDATE_SET_H_

#include <stdint.h>
#include <stdbool.h>

#include "rtdbd_select_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define RTDBD_CANDIDATE_SET_CAP 64u

typedef struct {
    rtdbd_source_candidate_t items[RTDBD_CANDIDATE_SET_CAP];
    uint32_t n;
    uint32_t duplicate_dropped;
    uint32_t truncated;
} rtdbd_candidate_set_t;

void rtdbd_candidate_set_init(rtdbd_candidate_set_t *s);
/* 返回 true 表示新入集；重复键 false 且 duplicate_dropped++；满额 false 且 truncated++。 */
bool rtdbd_candidate_set_add(rtdbd_candidate_set_t *s, const rtdbd_source_candidate_t *c);
/* 输出 rtdbd_source_candidate_t[]，返回实际条数（<=cap）。 */
uint32_t rtdbd_candidate_set_snapshot(const rtdbd_candidate_set_t *s,
                                      rtdbd_source_candidate_t *out, uint32_t cap);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_CANDIDATE_SET_H_ */
