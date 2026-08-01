/**
 * @file irt_shm.c
 * @brief 共享内存段管理实现
 * @version 3.1.0
 * @date 2026-07-31
 * @copyright MIT License
 *
 * v3.1: 新增崩溃恢复 — PID 存活检查接管所有权 + seqlock 奇数恢复
 */

#include "core/irt_shm.h"
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

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

    /* owner 负责初始化 header, 非 owner 校验 magic + 崩溃恢复 */
    irt_header_t* hdr = (irt_header_t*)s->base;

    if (irt_shm_os_is_owner(&s->os)) {
        memset(hdr, 0, sizeof(irt_header_t));
        hdr->magic          = IRT_MAGIC;
        hdr->version        = IRT_SHM_VERSION;
        hdr->max_points     = max_points;
        hdr->max_subscribers = max_subscribers;
        hdr->owner_pid      = (int32_t)getpid();
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

        /* ---- v3.1 崩溃恢复 ---- */

        /* 1. 检查原 owner 是否已死亡, 若死亡则接管所有权 */
        int32_t stored_pid = hdr->owner_pid;
        if (stored_pid > 0 && stored_pid != (int32_t)getpid()) {
            /* kill(pid, 0) 检查进程是否存在 (不发送信号).
             * ESRCH: 进程不存在 → 确认已死亡 → 可安全接管 */
            if (kill((pid_t)stored_pid, 0) != 0) {
                irt_shm_os_claim_ownership(&s->os);
                hdr->owner_pid = (int32_t)getpid();
            }
            /* 若原 owner 仍存活: 保持 attacher 身份, 不接管 */
        }

        /* 2. 恢复可能在锁内崩溃的 seqlock (奇数 = 写锁被遗留) */
        uint64_t seq = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
        if (seq & 1ULL) {
            /* 原子推进至下一个偶数: 释放遗留的写锁 */
            __atomic_store_n(&hdr->write_seq, seq + 1, __ATOMIC_RELEASE);
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
