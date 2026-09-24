#include "audit.h"

#include <string.h>

void irt_audit_init(irt_audit_t* a)
{
    if (!a) return;
    memset(a, 0, sizeof(*a));
}

void irt_audit_record(irt_audit_t* a, uint32_t pid, uint32_t uid,
                      uint32_t point_id, uint64_t ts_ns)
{
    if (!a) return;

    rtdbd_audit_entry_t* e = &a->entries[a->head];
    e->pid       = pid;
    e->uid       = uid;
    e->point_id  = point_id;
    e->ts_ns     = ts_ns;

    a->head = (a->head + 1u) % RTDBD_AUDIT_CAPACITY;
    if (a->count < RTDBD_AUDIT_CAPACITY) {
        a->count++;
    }
}

uint32_t irt_audit_dump(const irt_audit_t* a, rtdbd_audit_entry_t* out, uint32_t cap)
{
    if (!a || !out || cap == 0) return 0;

    uint32_t n = (a->count < cap) ? a->count : cap;
    /* 最旧的条目位于 head - count（环形） */
    uint32_t start = (a->head + RTDBD_AUDIT_CAPACITY - n) % RTDBD_AUDIT_CAPACITY;

    for (uint32_t i = 0; i < n; ++i) {
        out[i] = a->entries[(start + i) % RTDBD_AUDIT_CAPACITY];
    }
    return n;
}
