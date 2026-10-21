/**
 * @file irt_point_manager.c
 * @brief 点位管理器实现
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include "core/irt_point_manager.h"
#include <osal/irt_osal.h>
#include <internal/irt_seqlock.h>
#include <string.h>
#include <stdio.h>
#include "core/irt_meta_timeout.h"

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

static int pm_write_impl_q(irt_pm_t* pm, uint32_t id,
                           uint8_t type, const void* value,
                           uint64_t source_ts_ns, uint8_t quality) {
    if (!irt_pm_validate_id(pm, id)) return -1;

    irt_header_t*      hdr = irt_shm_header(pm->shm);
    indurtdb_point_t*  pts = irt_shm_points(pm->shm);
    if (!hdr || !pts) return -1;

    /* 写冲突有限重试。冲突窗口只有一次 seqlock 写入的时长（数纳秒），
     * 与 CRUD 路径 (h_point_register / load_config) 共用同一套
     * IRT_SEQLOCK_MAX_RETRY + sched_yield 模式。修复前本处直接 return -2，
     * 使并发写的丢写率与竞争强度成正比。 */
    uint64_t seq0 = 0;
    for (int attempt = 0; attempt < IRT_SEQLOCK_MAX_RETRY; ++attempt) {
        seq0 = irt_seqlock_write_begin(&hdr->write_seq);
        if (!(seq0 & 1ULL)) break;   /* 取到写锁 */
        sched_yield();
    }
    if (seq0 & 1ULL) return -2;      /* 重试耗尽仍冲突 */

    indurtdb_point_t* p = &pts[id];
    /* SRS §4.3: 只读点位禁止写入 */
    if (p->access == INDURTDB_ACCESS_READ_ONLY) {
        irt_seqlock_write_end(&hdr->write_seq, seq0);
        return -3;  /* read-only */
    }
    p->type = type;
    switch (type) {
    case INDURTDB_TYPE_BOOL:   p->value.b = *(const bool*)value;        break;
    case INDURTDB_TYPE_INT32:  p->value.i = *(const int32_t*)value;     break;
    case INDURTDB_TYPE_INT64:  p->value.i64 = *(const int64_t*)value;   break;
    case INDURTDB_TYPE_UINT32: p->value.u32 = *(const uint32_t*)value;  break;
    case INDURTDB_TYPE_FLOAT:  p->value.f = *(const float*)value;       break;
    case INDURTDB_TYPE_DOUBLE: p->value.d = *(const double*)value;      break;
    case INDURTDB_TYPE_STRING:
        strncpy(p->value.str, (const char*)value, 31);
        p->value.str[31] = '\0';
        break;
    default: break;
    }
    /* v3.9.1：入库时刻对外语义是「墙钟时间」，用 REALTIME 而非单调时钟，
     * 便于监控台/上位机直接渲染为日期。check_timeouts 必须使用同一时钟。 */
    p->timestamp_ns = irt_time_realtime_ns();
    p->quality      = quality;
    /* 采集时刻：0 表示"未提供"，语义退化为仅用入库时刻（旧行为不变） */
    p->source_timestamp_ns = source_ts_ns;

    /*
     * ARM weak memory ordering: 保证所有数据写入 (type/value/timestamp/
     * quality) 在 seqlock 释放前对其它 CPU 可见. x86 TSO 下是 no-op.
     */
    __atomic_thread_fence(__ATOMIC_RELEASE);
    irt_seqlock_write_end(&hdr->write_seq, seq0);
    __atomic_fetch_add(&hdr->stats.writes, 1, __ATOMIC_RELAXED);
    return 0;
}

/* 默认写入口：强制 GOOD，保持热路径纯净、零 meta 读（v3.7 不变式）。
 * 既有 irt_pm_write_*_ts 封装继续调用本函数，行为完全不变。 */
static int pm_write_impl(irt_pm_t* pm, uint32_t id, uint8_t type,
                         const void* value, uint64_t source_ts_ns) {
    return pm_write_impl_q(pm, id, type, value, source_ts_ns, INDURTDB_QUALITY_GOOD);
}

/* v3.7 主题A：质量感知写入口（写权威 rtdbd 使用），一次性落值 + 调用方质量 */
int irt_pm_write_quality_ts(irt_pm_t* pm, uint32_t id, uint8_t type,
                            const void* value, uint64_t source_ts_ns, uint8_t quality) {
    return pm_write_impl_q(pm, id, type, value, source_ts_ns, quality);
}

int irt_pm_write_bool(irt_pm_t* pm, uint32_t id, bool value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_BOOL, &value, 0);
}
int irt_pm_write_int32(irt_pm_t* pm, uint32_t id, int32_t value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_INT32, &value, 0);
}
int irt_pm_write_double(irt_pm_t* pm, uint32_t id, double value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_DOUBLE, &value, 0);
}
int irt_pm_write_int64(irt_pm_t* pm, uint32_t id, int64_t value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_INT64, &value, 0);
}
int irt_pm_write_uint32(irt_pm_t* pm, uint32_t id, uint32_t value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_UINT32, &value, 0);
}
int irt_pm_write_float(irt_pm_t* pm, uint32_t id, float value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_FLOAT, &value, 0);
}
int irt_pm_write_string(irt_pm_t* pm, uint32_t id, const char* value) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_STRING, value, 0);
}

/* ---- 携带采集时刻（SourceTimestamp）的写入 ---- */
int irt_pm_write_bool_ts(irt_pm_t* pm, uint32_t id, bool value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_BOOL, &value, source_ts_ns);
}
int irt_pm_write_int32_ts(irt_pm_t* pm, uint32_t id, int32_t value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_INT32, &value, source_ts_ns);
}
int irt_pm_write_double_ts(irt_pm_t* pm, uint32_t id, double value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_DOUBLE, &value, source_ts_ns);
}
int irt_pm_write_int64_ts(irt_pm_t* pm, uint32_t id, int64_t value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_INT64, &value, source_ts_ns);
}
int irt_pm_write_uint32_ts(irt_pm_t* pm, uint32_t id, uint32_t value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_UINT32, &value, source_ts_ns);
}
int irt_pm_write_float_ts(irt_pm_t* pm, uint32_t id, float value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_FLOAT, &value, source_ts_ns);
}
int irt_pm_write_string_ts(irt_pm_t* pm, uint32_t id, const char* value, uint64_t source_ts_ns) {
    return pm_write_impl(pm, id, INDURTDB_TYPE_STRING, value, source_ts_ns);
}

/* ---- 显式设置质量（如失联时标记 COMM_FAILURE） ---- */
int irt_pm_set_quality(irt_pm_t* pm, uint32_t id, uint8_t quality) {
    if (!irt_pm_validate_id(pm, id)) return -1;

    irt_header_t*     hdr = irt_shm_header(pm->shm);
    indurtdb_point_t* pts = irt_shm_points(pm->shm);
    if (!hdr || !pts) return -1;

    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return -2; /* 写冲突 */

    pts[id].quality = quality;

    __atomic_thread_fence(__ATOMIC_RELEASE);
    irt_seqlock_write_end(&hdr->write_seq, seq0);
    return 0;
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
    if (!pm) return 0;

    irt_header_t*      hdr = irt_shm_header(pm->shm);
    indurtdb_point_t*  pts = irt_shm_points(pm->shm);
    indurtdb_meta_t*   metas = irt_shm_meta(pm->shm);
    if (!hdr || !pts || !metas) return 0;

    /* 必须与写入 timestamp_ns 使用同一时钟（REALTIME），否则跨时钟相减无意义。 */
    uint64_t now = irt_time_realtime_ns();
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

        /* v3.10: 计算有效超时（纳秒）。
         * - 点位启用 per-point 且 timeout_ms > 0 时用其值；
         * - 否则用全局 timeout_ns；
         * - 有效超时为 0 时跳过（不检测该点）。 */
        uint32_t pt_timeout_ms = irt_meta_timeout_ms(&metas[id]);
        uint64_t effective_ns = (pt_timeout_ms > 0) ? (uint64_t)pt_timeout_ms * 1000000ULL : timeout_ns;
        if (effective_ns == 0) continue;

        /* 超时检测: ts > now 表示扫描期间被并发写入更新过, 跳过;
         * 使用 now - ts <= effective_ns 而非 ts + effective_ns >= now,
         * 避免 ts + effective_ns 在极端超时值下溢出. */
        if (ts >= now) continue;
        if (now - ts <= effective_ns) continue;

        /* 获取写锁, 标记 TIMEOUT */
        uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
        if (seq0 & 1ULL) {
            /* 让步退避后仍冲突: 该点本轮被跳过, 计入可观测计数 (issue #19 L2) */
            __atomic_fetch_add(&hdr->scan_skipped, 1, __ATOMIC_RELAXED);
            continue;
        }

        indurtdb_point_t* p = &pts[id];
        /* 二次确认: 可能在等待写锁期间被其它线程更新了 */
        if (p->timestamp_ns == 0
            || p->timestamp_ns >= now
            || p->quality == INDURTDB_QUALITY_TIMEOUT
            || now - p->timestamp_ns <= effective_ns) {
            irt_seqlock_write_end(&hdr->write_seq, seq0);
            continue;
        }

        /* 超时刻保留量程位（T6：量程位与可用性正交，不得被基础码覆盖） */
        p->quality = INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_TIMEOUT,
                                           INDURTDB_QUALITY_LIMIT(p->quality));
        __atomic_thread_fence(__ATOMIC_RELEASE);
        irt_seqlock_write_end(&hdr->write_seq, seq0);
        __atomic_fetch_add(&hdr->stats.timeouts, 1, __ATOMIC_RELAXED);
        detected++;
    }
    return detected;
}
