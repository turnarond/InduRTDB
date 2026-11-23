/**
 * @file hB_audit_adapter.h
 * @brief h-B 生产审计回调（独立可测 TU）：把 VSOA 审计回调从 rtdbd.c static
 * 提取到此处导出，使单测能直接调用**生产回调本身**（而非测试侧仿制品）。
 *
 * t-A~t-E 裁决：t-B 要求审计可追溯——service_id 写纯内存 sid 环（非阻塞），
 * 审计条目仍走 irt_audit_record（24B 协议冻结、无 service_id 字段）。
 */
#ifndef HB_AUDIT_ADAPTER_H_
#define HB_AUDIT_ADAPTER_H_

#include "audit.h"
#include "hB_audit_sid.h"

#include <stdint.h>

typedef struct {
    irt_audit_t          *audit;    /* 24B 审计条目环（生产 &audit） */
    hB_audit_sid_ring_t  *sid_ring; /* service_id 旁路环（纯内存、非阻塞） */
} hB_audit_ctx_t;

/* 符合 rtdbd_vsoa_audit_fn 签名的生产回调。ts_ns==0 时用 CLOCK_MONOTONIC 补。
 * pid 恒 0（VSOA hello 不赋）如实记录；网关 deny 路径不会调用本回调。 */
void hB_audit_callback(void *ctx, uint32_t pid, uint32_t uid,
                       uint32_t partition, uint32_t point_id,
                       uint64_t ts_ns, const char *service_id);

#endif /* HB_AUDIT_ADAPTER_H_ */
