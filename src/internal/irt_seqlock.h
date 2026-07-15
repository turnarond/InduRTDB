/**
 * @file irt_seqlock.h
 * @brief Seqlock 自由函数 (直译自 v2.x seqlock.hpp)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_INTERNAL_IRT_SEQLOCK_H_
#define IRT_INTERNAL_IRT_SEQLOCK_H_

#include "irt_types.h"

static inline uint64_t irt_seqlock_write_begin(uint64_t* seq) {
    uint64_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s & 1ULL) return s;                          /* 奇数 = 写冲突 */
    __atomic_store_n(seq, s + 1, __ATOMIC_RELEASE);  /* 标记写入中 */
    return s;
}

static inline void irt_seqlock_write_end(uint64_t* seq, uint64_t seq0) {
    __atomic_store_n(seq, seq0 + 2, __ATOMIC_RELEASE);
}

static inline const indurtdb_point_t* irt_seqlock_read(
    const uint64_t* seq, const indurtdb_point_t* points, uint32_t id) {
    uint64_t s0, s1;
    s0 = 0; s1 = 1;   /* 初始化不同, 确保至少循环一次 */
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                     /* 写中, 重试 */
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return &points[id];
}

#endif /* IRT_INTERNAL_IRT_SEQLOCK_H_ */
