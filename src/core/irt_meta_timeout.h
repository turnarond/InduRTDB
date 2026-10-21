/**
 * @file irt_meta_timeout.h
 * @brief Per-point timeout accessors for indurtdb_meta_t
 * @version 3.10.0
 * @date 2026-10-21
 * @copyright MIT License
 *
 * 使用 memcpy 读写 reserved[0..3]，避免对齐/别名问题。
 * reserved[4..7] 必须保持为 0（由校验层保证）。
 */

#ifndef IRT_CORE_IRT_META_TIMEOUT_H_
#define IRT_CORE_IRT_META_TIMEOUT_H_

#include <stdint.h>
#include <string.h>
#include <indurtdb/indurtdb.h>

/* 清除 timeout：flags 清 bit3，reserved[0..3] 置 0。reserved[4..7] 不动。 */
static inline void irt_meta_clear_timeout(indurtdb_meta_t* m)
{
    if (!m) return;
    m->flags &= ~INDURTDB_META_FLAG_TIMEOUT;
    m->reserved[0] = 0;
    m->reserved[1] = 0;
    m->reserved[2] = 0;
    m->reserved[3] = 0;
}

/* 从 reserved[0..3] 读取 timeout_ms（小端）。flags 未置 bit3 时返回 0。 */
static inline uint32_t irt_meta_timeout_ms(const indurtdb_meta_t* m)
{
    if (!m) return 0;
    if (!(m->flags & INDURTDB_META_FLAG_TIMEOUT)) return 0;
    uint32_t v = 0;
    memcpy(&v, m->reserved, sizeof(v));
    return v;
}

/* 向 reserved[0..3] 写入 timeout_ms（小端），并置位 flags bit3。
 * ms == 0 时清除 flag 与 reserved[0..3]（等同 irt_meta_clear_timeout）。 */
static inline void irt_meta_set_timeout_ms(indurtdb_meta_t* m, uint32_t ms)
{
    if (!m) return;
    if (ms == 0) {
        irt_meta_clear_timeout(m);
        return;
    }
    m->flags |= INDURTDB_META_FLAG_TIMEOUT;
    memcpy(m->reserved, &ms, sizeof(ms));
    /* reserved[4..7] 保持为 0（调用方应确保 meta 初始化为零） */
}

#endif /* IRT_CORE_IRT_META_TIMEOUT_H_ */