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
#include "core/irt_index.h"
#include <internal/irt_seqlock.h>
#include <string.h>
#include <stddef.h>
#include <stdio.h>
#include <unistd.h>
#include <signal.h>

/* ---- v3.4 布局 v2：Header CRC32 ----
 * 位运算实现（无查表、无静态存储），避免共享内存库引入额外内存占用。
 * 多项式 0xEDB88320（IEEE 802.3），初值/结果异或 0xFFFFFFFF。 */
static uint32_t irt_crc32_update(uint32_t crc, const void* data, size_t len)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
        }
    }
    return crc;
}

/* 计算 Header 的 CRC。
 *
 * **覆盖范围：仅布局描述字段**，即创建后不再变化的部分：
 *   magic / version / max_points / max_subscribers  (0..15)
 *   flags / off_points / off_index / off_meta / off_subs (48..71)
 *
 * 刻意**排除**易变字段：write_seq（seqlock 每写递增）、owner_pid（崩溃接管会改）、
 * stats（每次读写累加）、scan_skipped。若把它们纳入，段一旦被写过 CRC 即失效，
 * attach 会永远失败 —— 这是 T1 绿阶段实测踩到的真实错误。
 *
 * 约束：任何修改布局描述字段的路径，修改后必须重新 irt_header_seal()，
 * 否则 attach 侧会以 CRC 失败拒绝挂载。 */
static uint32_t irt_header_crc32_of(const irt_header_t* h)
{
    const uint8_t* raw = (const uint8_t*)h;
    uint32_t crc = 0xFFFFFFFFu;

    crc = irt_crc32_update(crc, raw + offsetof(irt_header_t, magic), 16u);
    crc = irt_crc32_update(crc, raw + offsetof(irt_header_t, flags), 24u);
    return crc ^ 0xFFFFFFFFu;
}

int irt_header_seal(irt_header_t* h)
{
    if (!h) return -1;
    h->crc32 = 0u;
    h->crc32 = irt_header_crc32_of(h);
    return 0;
}

int irt_header_verify(const irt_header_t* h)
{
    if (!h) return 0;
    if (h->magic != IRT_MAGIC) return 0;
    return irt_header_crc32_of(h) == h->crc32 ? 1 : 0;
}

int irt_shm_init(irt_shm_t* s, const char* instance_id,
                 uint32_t max_points, uint32_t max_subscribers) {
    if (!s || !instance_id || instance_id[0] == '\0'
        || max_points == 0) return IRT_SHM_ERR_ARG;

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
        /* 布局 v2：区段偏移写进 Header，访问点一律读 off_* */
        hdr->off_points = irt_layout_off_points();
        hdr->off_index  = irt_layout_off_index(max_points);
        hdr->off_meta   = irt_layout_off_meta(max_points);
        hdr->off_subs   = irt_layout_off_subs(max_points);
        irt_header_seal(hdr);
        /* 索引区置空 (EMPTY = UINT32_MAX), 随段一起交付给 attacher */
        irt_index_clear(s);
        /* 元数据区清零: 默认 eur_min/max=0, deadband=0, flags=0 */
        {
            indurtdb_meta_t* meta = irt_shm_meta(s);
            if (meta) memset(meta, 0, irt_layout_meta_size(max_points));
        }
    } else {
        /* attach: 先判版本/布局兼容性。
         * 版本不匹配必须**拒绝挂载**：旧段按新布局解释会读到错误偏移，
         * 属于静默数据错乱。调用方据此走迁移流程（v3.4 迁移工具）。 */
        if (hdr->magic   != IRT_MAGIC
         || hdr->version != IRT_SHM_VERSION) {
            irt_shm_os_unmap(&s->os);
            memset(s, 0, sizeof(*s));
            return IRT_SHM_ERR_VERSION;
        }
        if (hdr->max_points     < max_points
         || hdr->max_subscribers < max_subscribers) {
            irt_shm_os_unmap(&s->os);
            memset(s, 0, sizeof(*s));
            return IRT_SHM_ERR_ARG;
        }
        if (!irt_header_verify(hdr)) {
            irt_shm_os_unmap(&s->os);
            memset(s, 0, sizeof(*s));
            return IRT_SHM_ERR_CRC;
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
    if (!hdr) return NULL;
    /* 布局 v2：按 Header 中的 off_points，不再按 sizeof(header) 硬算 */
    if (hdr->off_points == 0) return NULL;   /* 未 seal 的裸段，拒绝解释 */
    return (indurtdb_point_t*)((char*)hdr + hdr->off_points);
}

irt_subscriber_entry_t* irt_shm_subscribers(const irt_shm_t* s) {
    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr || s->max_subscribers == 0) return NULL;
    if (hdr->off_subs == 0) return NULL;
    return (irt_subscriber_entry_t*)((char*)hdr + hdr->off_subs);
}

/* ---- v3.4 T3: 元数据区 (每点 32B, 冷数据) ---- */

indurtdb_meta_t* irt_shm_meta(const irt_shm_t* s) {
    irt_header_t* hdr = irt_shm_header(s);
    if (!hdr || hdr->off_meta == 0) return NULL;
    return (indurtdb_meta_t*)((char*)hdr + hdr->off_meta);
}

int irt_meta_set(irt_shm_t* s, uint32_t id, const indurtdb_meta_t* m) {
    if (!s || !m) return INDURTDB_ERR_ARG;
    if (id >= s->max_points) return INDURTDB_ERR_ARG;

    indurtdb_meta_t* meta = irt_shm_meta(s);
    if (!meta) return INDURTDB_ERR_ARG;

    irt_header_t* hdr = irt_shm_header(s);
    uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
    if (seq0 & 1ULL) return INDURTDB_ERR_BUSY;

    memcpy(&meta[id], m, sizeof(indurtdb_meta_t));
    __atomic_thread_fence(__ATOMIC_RELEASE);
    irt_seqlock_write_end(&hdr->write_seq, seq0);
    return 0;
}

int irt_meta_get(irt_shm_t* s, uint32_t id, indurtdb_meta_t* out) {
    if (!s || !out) return INDURTDB_ERR_ARG;
    if (id >= s->max_points) return INDURTDB_ERR_ARG;

    indurtdb_meta_t* meta = irt_shm_meta(s);
    if (!meta) return INDURTDB_ERR_ARG;

    irt_header_t* hdr = irt_shm_header(s);
    /* seqlock 读重试: 拷贝在窗口内, 窗口前后 seq 一致才采信 */
    for (int attempt = 0; attempt < 16; ++attempt) {
        uint64_t s0 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;
        memcpy(out, &meta[id], sizeof(indurtdb_meta_t));
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        uint64_t s1 = __atomic_load_n(&hdr->write_seq, __ATOMIC_ACQUIRE);
        if (s0 == s1) return 0;
    }
    return INDURTDB_ERR_BUSY;
}
