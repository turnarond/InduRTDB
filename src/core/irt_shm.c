/**
 * @file irt_shm.c
 * @brief 共享内存段管理实现
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_shm.h"
#include <string.h>
#include <stdio.h>

int irt_shm_init(irt_shm_t* s, const char* instance_id,
                 uint32_t max_points, uint32_t max_subscribers) {
    if (!s || !instance_id || instance_id[0] == '\0'
        || max_points == 0) return -1;

    memset(s, 0, sizeof(*s));
    s->max_points      = max_points;
    s->max_subscribers = max_subscribers;
    s->total_size      = irt_shm_total_size(max_points, max_subscribers);

    char name[128];
    int n = snprintf(name, sizeof(name), "%s%s", IRT_SHM_PREFIX, instance_id);
    if (n < 0 || (size_t)n >= sizeof(name)) return -1;

    s->base = irt_shm_os_map(&s->os, name, s->total_size);
    if (!s->base) return -1;

    /* owner 负责初始化 header, 非 owner 校验 magic */
    irt_header_t* hdr = (irt_header_t*)s->base;
    if (irt_shm_os_is_owner(&s->os)) {
        memset(hdr, 0, sizeof(irt_header_t));
        hdr->magic          = IRT_MAGIC;
        hdr->version        = IRT_SHM_VERSION;
        hdr->max_points     = max_points;
        hdr->max_subscribers = max_subscribers;
    } else {
        /* attach: 校验段兼容性 */
        if (hdr->magic    != IRT_MAGIC
         || hdr->version  != IRT_SHM_VERSION
         || hdr->max_points     < max_points
         || hdr->max_subscribers < max_subscribers) {
            irt_shm_os_unmap(&s->os);
            memset(s, 0, sizeof(*s));
            return -1;
        }
    }
    return 0;
}

void irt_shm_shutdown(irt_shm_t* s) {
    if (!s) return;
    irt_shm_os_unmap(&s->os);
    memset(s, 0, sizeof(*s));
}

bool irt_shm_is_owner(const irt_shm_t* s) {
    return s ? irt_shm_os_is_owner(&s->os) : false;
}

irt_header_t* irt_shm_header(const irt_shm_t* s) {
    return s ? (irt_header_t*)s->base : NULL;
}

indurtdb_point_t* irt_shm_points(const irt_shm_t* s) {
    irt_header_t* hdr = irt_shm_header(s);
    return hdr ? (indurtdb_point_t*)((char*)hdr + sizeof(irt_header_t)) : NULL;
}

irt_subscriber_entry_t* irt_shm_subscribers(const irt_shm_t* s) {
    if (!s || s->max_subscribers == 0) return NULL;
    indurtdb_point_t* pts = irt_shm_points(s);
    if (!pts) return NULL;
    return (irt_subscriber_entry_t*)((char*)pts
           + (size_t)s->max_points * sizeof(indurtdb_point_t));
}
