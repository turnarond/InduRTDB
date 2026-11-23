/**
 * @file hB_audit_sid.c
 * @brief h-B 审计 service_id 旁路纯内存环实现（见 hB_audit_sid.h）。
 */
#include "hB_audit_sid.h"

#include <string.h>

void hB_audit_sid_ring_init(hB_audit_sid_ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}

void hB_audit_sid_ring_push(hB_audit_sid_ring_t *r, uint64_t ts_ns,
                            uint32_t uid, uint32_t partition, uint32_t point_id,
                            const char *service_id)
{
    if (!r) return;
    if (service_id && strlen(service_id) >= sizeof(r->entries[0].service_id)) {
        r->rejected++; /* 超长标识：截断写入并计数，不静默 */
    }
    hB_audit_sid_entry_t *e = &r->entries[r->head];
    e->ts_ns = ts_ns;
    e->uid = uid;
    e->partition = partition;
    e->point_id = point_id;
    if (service_id) {
        size_t n = sizeof(e->service_id) - 1u;
        strncpy(e->service_id, service_id, n); /* 定长截断，末字节下面置 '\0' */
        e->service_id[sizeof(e->service_id) - 1u] = '\0';
    } else {
        e->service_id[0] = '\0';
    }
    r->head = (r->head + 1u) % HB_AUDIT_SID_CAPACITY;
    if (r->count < HB_AUDIT_SID_CAPACITY) r->count++;
}

uint32_t hB_audit_sid_ring_dump(const hB_audit_sid_ring_t *r,
                                hB_audit_sid_entry_t *out, uint32_t cap)
{
    if (!r || !out || cap == 0u) return 0u;
    uint32_t n = r->count < cap ? r->count : cap;
    /* head 指向下一个写入位；最旧条目 = (head - count + CAP) % CAP */
    uint32_t start = (r->head + HB_AUDIT_SID_CAPACITY - r->count) % HB_AUDIT_SID_CAPACITY;
    for (uint32_t i = 0u; i < n; ++i) {
        out[i] = r->entries[(start + i) % HB_AUDIT_SID_CAPACITY];
    }
    return n;
}

void hB_audit_sid_ring_clear(hB_audit_sid_ring_t *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
}
