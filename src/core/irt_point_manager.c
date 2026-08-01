/**
 * @file irt_point_manager.c
 * @brief 点位管理器实现
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_point_manager.h"
#include <osal/irt_osal.h>
#include <internal/irt_seqlock.h>
#include <string.h>
#include <stdio.h>

void irt_pm_init(irt_pm_t* pm, irt_shm_t* shm) {
    if (!pm || !shm) return;
    pm->shm        = shm;
    pm->max_points = shm->max_points;
}

bool irt_pm_validate_id(const irt_pm_t* pm, uint32_t id) {
    if (!pm || !pm->shm) return false;
    irt_header_t* hdr = irt_shm_header(pm->shm);
    return hdr && id < hdr->max_points;
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
    /* SRS §4.3: 只读点位禁止写入 */
    if (p->access == INDURTDB_ACCESS_READ_ONLY) {
        irt_seqlock_write_end(&hdr->write_seq, seq0);
        return -3;  /* read-only */
    }
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

    /*
     * ARM weak memory ordering: 保证所有数据写入 (type/value/timestamp/
     * quality) 在 seqlock 释放前对其它 CPU 可见. x86 TSO 下是 no-op.
     */
    __atomic_thread_fence(__ATOMIC_RELEASE);
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

    /* 标准 seqlock 读模式: 在重试循环内拷贝数据, 避免 TOCTOU 脏读 */
    uint64_t* seq = &irt_shm_header(pm->shm)->write_seq;
    uint64_t s0 = 0, s1 = 1;
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                     /* 写中, 重试 */
        memcpy(out, &pts[id], sizeof(indurtdb_point_t));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return 0;
}

const indurtdb_point_t* irt_pm_peek(irt_pm_t* pm, uint32_t id) {
    if (!irt_pm_validate_id(pm, id)) return NULL;
    indurtdb_point_t* pts = irt_shm_points(pm->shm);
    if (!pts) return NULL;

    /* 标准 seqlock 读模式拷贝到线程本地缓冲区, 避免 TOCTOU 脏读.
     * _Thread_local 保证每线程独立, 无竞态; 调用方应在下次 peek 前用完数据. */
    static _Thread_local indurtdb_point_t buf;
    uint64_t* seq = &irt_shm_header(pm->shm)->write_seq;
    uint64_t s0 = 0, s1 = 1;
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                     /* 写中, 重试 */
        memcpy(&buf, &pts[id], sizeof(indurtdb_point_t));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return &buf;
}

uint64_t irt_pm_write_count(const irt_pm_t* pm) {
    irt_header_t* hdr = irt_shm_header(pm->shm);
    return hdr ? __atomic_load_n(&hdr->stats.writes, __ATOMIC_RELAXED) : 0;
}

int irt_pm_check_timeouts(irt_pm_t* pm, uint64_t timeout_ns) {
    if (!pm || timeout_ns == 0) return 0;

    irt_header_t*      hdr = irt_shm_header(pm->shm);
    indurtdb_point_t*  pts = irt_shm_points(pm->shm);
    if (!hdr || !pts) return 0;

    uint64_t now = irt_time_now_ns();
    /* 时钟获取失败(返回0)时跳过超时检测, 避免将所有点标记为 TIMEOUT */
    if (now == 0) return 0;
    uint32_t maxp = hdr->max_points;
    int detected = 0;

    for (uint32_t id = 0; id < maxp; id++) {
        /* 标准 seqlock 读模式: 在重试循环内读取 timestamp 和 quality, 避免 TOCTOU 脏读 */
        uint64_t ts = 0;
        uint8_t  q  = 0;
        {
            uint64_t s0 = 0, s1 = 1;
            do {
                s0 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
                if (s0 & 1ULL) continue;                     /* 写中, 重试 */
                ts = pts[id].timestamp_ns;
                q  = pts[id].quality;
                __atomic_thread_fence(__ATOMIC_ACQUIRE);
                s1 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
            } while (s0 != s1);
        }

        /* 跳过从未写入或已经标记为 TIMEOUT 的点 */
        if (ts == 0) continue;
        if (q == INDURTDB_QUALITY_TIMEOUT) continue;

        /* 超时检测: ts > now 表示扫描期间被并发写入更新过, 跳过;
         * 使用 now - ts <= timeout_ns 而非 ts + timeout_ns >= now,
         * 避免 ts + timeout_ns 在极端超时值下溢出. */
        if (ts >= now) continue;
        if (now - ts <= timeout_ns) continue;

        /* 获取写锁, 标记 TIMEOUT */
        uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
        if (seq0 & 1ULL) continue;  /* 写冲突, 跳过 */

        indurtdb_point_t* p = &pts[id];
        /* 二次确认: 可能在等待写锁期间被其它线程更新了 */
        if (p->timestamp_ns == 0
            || p->timestamp_ns >= now
            || p->quality == INDURTDB_QUALITY_TIMEOUT
            || now - p->timestamp_ns <= timeout_ns) {
            irt_seqlock_write_end(&hdr->write_seq, seq0);
            continue;
        }

        p->quality = INDURTDB_QUALITY_TIMEOUT;
        __atomic_thread_fence(__ATOMIC_RELEASE);
        irt_seqlock_write_end(&hdr->write_seq, seq0);
        __atomic_fetch_add(&hdr->stats.timeouts, 1, __ATOMIC_RELAXED);
        detected++;
    }
    return detected;
}
