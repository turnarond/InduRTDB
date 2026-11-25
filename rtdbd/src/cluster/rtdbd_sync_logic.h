/**
 * @file rtdbd_sync_logic.h
 * @brief S2 同步通道 cluster 层纯逻辑（无 SDK/无 fd，unit 可直测）。
 *
 * 只做：delta 扫描游标水位、snapshot 分页计数、WAIT ack 阈值判定。
 * 真实 wal / backlog / 全量源裁决留 S3；本层不宣称增量正确性。
 */
#ifndef RTDBD_SYNC_LOGIC_H_
#define RTDBD_SYNC_LOGIC_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include <indurtdb/indurtdb.h>
#include "vsoa/vsoa_sync_protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

/* delta 扫描：从 from_offset（=上一轮 applied_offset）起按 id 升序扫已注册点，
 * 每点生成一条 rtdbd_sync_delta_rec_t。返回写入条数；applied 为下轮起点、
 * has_more=是否还有未扫点。可证伪语义：连续两轮记录的 point_id 不重不漏。
 * owner_node/boot_id 由调用方注入（本机身份）。 */
uint32_t rtdbd_sync_delta_scan(indurtdb_t *h, uint32_t max_points,
                               uint64_t from_offset, uint32_t max_records, uint64_t epoch,
                               const char *node_id, uint64_t boot_id,
                               rtdbd_sync_delta_rec_t *recs, uint32_t cap,
                               uint64_t *applied, bool *has_more);

/* snapshot 页数：向上取整；n_points=0 → 0 页。 */
uint32_t rtdbd_sync_page_count(uint32_t n_points, uint32_t page_capacity);

/* WAIT ack 阈值：peer_epochs 中 >= min_epoch 的个数即 acks；ok=(acks>=required)。
 * 超时不回滚由 adapter pump 处理（本函数不阻塞、不做超时）。 */
void rtdbd_sync_wait_eval(const uint64_t *peer_epochs, uint32_t n_peers,
                          uint64_t min_epoch, uint32_t required_acks,
                          uint32_t *acks, bool *ok);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_SYNC_LOGIC_H_ */
