/**
 * @file irt_point_manager.c
 * @brief 点位管理器实现
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_point_manager.h"
#include "osal/irt_osal.h"
#include "internal/irt_seqlock.h"
#include <string.h>
#include <stdio.h>

void irt_pm_init(irt_pm_t* pm, irt_shm_t* shm) {
    if (!pm || !shm) return;
    pm->shm        = shm;
    pm->max_points = shm->max_points;
}

bool irt_pm_validate_id(const irt_pm_t* pm, uint32_t id) {
    return pm && id < pm->max_points;
}

static int pm_write_impl(irt_pm_t* pm, uint32_t id,
                          uint8_t type, const void* value) {
    if (!irt_pm_validate_id(pm, id)) return -1;

    irt_header_t*      hdr = irt_shm_header(pm->shm);
    indurtdb_point_t*  pts = irt_shm_points(pm->shm);
    if (!hdr || !pts) return -1;

    /* seqlock write begin */
    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return -2;  /* 写冲突 */

    indurtdb_point_t* p = &pts[id];
    p->type = type;
    switch (type) {
    case INDURTDB_TYPE_BOOL:   p->value.b = *(const bool*)value;     break;
    case INDURTDB_TYPE_INT32:  p->value.i = *(const int32_t*)value;  break;
    case INDURTDB_TYPE_DOUBLE: p->value.d = *(const double*)value;   break;
    case INDURTDB_TYPE_STRING:
        strncpy(p->value.str, (const char*)value, 31);
        p->value.str[31] = '\0';
        break;
    default: break;
    }
    p->timestamp_ns = irt_time_now_ns();
    p->quality      = INDURTDB_QUALITY_GOOD;

    irt_seqlock_write_end(&hdr->write_seq, seq0);
    __atomic_fetch_add(&hdr->stats.writes, 1, __ATOMIC_RELAXED);
    return 0;
}

int irt_pm_write_bool(irt_pm_t* pm, uint32_t id, bool value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_BOOL, &value);
}
int irt_pm_write_int32(irt_pm_t* pm, uint32_t id, int32_t value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_INT32, &value);
}
int irt_pm_write_double(irt_pm_t* pm, uint32_t id, double value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_DOUBLE, &value);
}
int irt_pm_write_string(irt_pm_t* pm, uint32_t id, const char* value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_STRING, value);
}

int irt_pm_read(irt_pm_t* pm, uint32_t id, indurtdb_point_t* out) {
    if (!irt_pm_validate_id(pm, id) || !out) return -1;
    indurtdb_point_t* pts = irt_shm_points(pm->shm);
    if (!pts) return -1;

    /* 使用统一的 irt_seqlock_read 获取安全指针, 再拷贝 */
    const indurtdb_point_t* src = irt_seqlock_read(
        &irt_shm_header(pm->shm)->write_seq, pts, id);
    if (!src) return -1;
    memcpy(out, src, sizeof(indurtdb_point_t));
    return 0;
}

const indurtdb_point_t* irt_pm_peek(irt_pm_t* pm, uint32_t id) {
    if (!irt_pm_validate_id(pm, id)) return NULL;
    indurtdb_point_t* pts = irt_shm_points(pm->shm);
    if (!pts) return NULL;
    return (const indurtdb_point_t*)irt_seqlock_read(
        &irt_shm_header(pm->shm)->write_seq, pts, id);
}

uint64_t irt_pm_write_count(const irt_pm_t* pm) {
    irt_header_t* hdr = irt_shm_header(pm->shm);
    return hdr ? __atomic_load_n(&hdr->stats.writes, __ATOMIC_RELAXED) : 0;
}
