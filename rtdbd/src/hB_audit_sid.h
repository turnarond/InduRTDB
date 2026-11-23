/**
 * @file hB_audit_sid.h
 * @brief h-B t-B 审计可追溯：service_id 旁路**纯内存**环形通道。
 *
 * 为什么旁路：24B rtdbd_audit_entry_t 无 service_id 字段（协议冻结），且
 * irt_logbuf_emit 有 fprintf+fflush（阻塞，违反 vsoa_adapter.c:10 回调不阻塞
 * 硬约束）。故审计回调把 service_id + 身份写入本纯内存环，零 I/O、零阻塞；
 * VSOA 写审计条目（irt_audit）与本 sid 环按 uid/partition/point_id + 邻近 ts 关联。
 *
 * 定长 256 条、无堆分配；单线程事件循环内使用（与 audit 同约束），非线程安全。
 */
#ifndef HB_AUDIT_SID_H_
#define HB_AUDIT_SID_H_

#include <stdint.h>

#define HB_AUDIT_SID_CAPACITY 256u

typedef struct {
    uint64_t ts_ns;              /* now_ns()/CLOCK_MONOTONIC，与审计条目同源 */
    uint32_t uid;
    uint32_t partition;
    uint32_t point_id;
    char     service_id[64];     /* VSOA 侧逻辑身份；UDS 侧为空串 */
} hB_audit_sid_entry_t;

typedef struct {
    hB_audit_sid_entry_t entries[HB_AUDIT_SID_CAPACITY];
    uint32_t head;
    uint32_t count;
    uint64_t rejected;           /* 参数非法被拒（service_id 过长等），不静默 */
} hB_audit_sid_ring_t;

void     hB_audit_sid_ring_init(hB_audit_sid_ring_t *r);
/* 纯内存写入：ts_ns==0 时由调用方（生产回调）已补 now_ns；本函数不再取时钟。
 * service_id 超长（>=64）截断并计 rejected。 */
void     hB_audit_sid_ring_push(hB_audit_sid_ring_t *r, uint64_t ts_ns,
                                uint32_t uid, uint32_t partition, uint32_t point_id,
                                const char *service_id);
/* 按最旧→最新导出，返回实际条数。 */
uint32_t hB_audit_sid_ring_dump(const hB_audit_sid_ring_t *r,
                                hB_audit_sid_entry_t *out, uint32_t cap);
void     hB_audit_sid_ring_clear(hB_audit_sid_ring_t *r);

#endif /* HB_AUDIT_SID_H_ */
