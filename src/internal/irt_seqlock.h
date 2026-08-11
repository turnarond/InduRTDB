/**
 * @file irt_seqlock.h
 * @brief Seqlock 自由函数 (直译自 v2.x seqlock.hpp)
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_INTERNAL_IRT_SEQLOCK_H_
#define IRT_INTERNAL_IRT_SEQLOCK_H_

#include "irt_types.h"

/* ---- 写端 (CAS 循环) ---- */
static inline uint64_t irt_seqlock_write_begin(uint64_t* seq) {
    uint64_t expected = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    while (1) {
        if (expected & 1ULL) return expected;  /* 奇数 = 写冲突 */
        if (__atomic_compare_exchange_n(seq, &expected, expected + 1,
                /*weak=*/false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
            return expected;  /* 成功获取写锁, 返回进入前的偶数seq */
        }
        /* CAS 失败: expected 已被更新为当前seq值, 重试 */
    }
}

static inline void irt_seqlock_write_end(uint64_t* seq, uint64_t seq0) {
    __atomic_store_n(seq, seq0 + 2, __ATOMIC_RELEASE);
}

/* 读端: 调用方自行在重试循环内读取数据, 模式如下:
 *   uint64_t s0, s1;
 *   do {
 *       s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
 *       if (s0 & 1ULL) continue;
 *       // 读取数据 (memcpy / 字段访问)
 *       __atomic_thread_fence(__ATOMIC_ACQUIRE);
 *       s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
 *   } while (s0 != s1);
 * irt_seqlock_read() 已移除: 返回裸指针导致调用方在验证窗之外读数据, 存在 TOCTOU 脏读. */

#endif /* IRT_INTERNAL_IRT_SEQLOCK_H_ */
