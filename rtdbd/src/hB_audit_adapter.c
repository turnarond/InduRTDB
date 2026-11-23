/**
 * @file hB_audit_adapter.c
 * @brief h-B 生产审计回调实现（见 hB_audit_adapter.h）。
 */
#include "hB_audit_adapter.h"

#include <time.h>

static uint64_t hB_mono_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

void hB_audit_callback(void *ctx, uint32_t pid, uint32_t uid,
                       uint32_t partition, uint32_t point_id,
                       uint64_t ts_ns, const char *service_id)
{
    hB_audit_ctx_t *c = (hB_audit_ctx_t *)ctx;
    if (!c || !c->audit) return; /* 无审计缓冲：不静默伪装成功由上层计数体现 */
    if (ts_ns == 0u) ts_ns = hB_mono_now_ns(); /* adapter 传 0，补单调钟（与审计条目同源） */

    /* 参数顺序：irt_audit_record(a, pid, uid, partition, point_id, ts)
     * —— partition 在 point_id 前（audit.h:20-21），二者均 uint32，写反编译器不报错。 */
    irt_audit_record(c->audit, pid, uid, partition, point_id, ts_ns);

    /* t-B 可追溯：service_id 走纯内存 sid 环，零 I/O、非阻塞（不碰 logbuf）。 */
    if (c->sid_ring) {
        hB_audit_sid_ring_push(c->sid_ring, ts_ns, uid, partition, point_id, service_id);
    }
    (void)pid; /* pid 恒 0 已随审计条目如实记录 */
}
