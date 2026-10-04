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
#include <sched.h>

#ifndef IRT_SEQLOCK_MAX_RETRY
#define IRT_SEQLOCK_MAX_RETRY 3   /* 写锁冲突有限重试上限 (issue #19 L1) */
#endif

/* ---- 写端 (CAS 循环 + 冲突退避) ----
 *
 * 冲突（他人持写锁，seq 为奇数）时不再立即返回，而是有限次退避 + 让出 CPU，
 * 给持锁方一个完成窗口。这是 issue #19 (L1) 的核心修复：热写者自旋下，
 * 超时扫描（同样走写锁）不再整轮取不到锁而饿死。
 * 退避有上限（IRT_SEQLOCK_MAX_RETRY），避免单次扫描被拖垮（见设计文档风险登记）。 */
static inline uint64_t irt_seqlock_write_begin(uint64_t* seq) {
    uint64_t expected = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    int retry = 0;
    for (;;) {
        if (!(expected & 1ULL)) {
            if (__atomic_compare_exchange_n(seq, &expected, expected + 1,
                    /*weak=*/false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
                return expected;  /* 成功获取写锁, 返回进入前的偶数seq */
            }
            /* CAS 失败: expected 已被更新为当前值, 继续抢锁 (偶数竞争) */
            retry = 0;
            continue;
        }
        /* 奇数 = 他人持写锁. 有限退避 + 让出 CPU, 给持锁方完成窗口,
         * 避免热写者自旋下对方被饿死 (issue #19 L1). */
        if (++retry > IRT_SEQLOCK_MAX_RETRY) return expected;  /* 冲突, 返回奇数 */
        /* 指数退避: 2^retry 次极轻量自旋后让出 CPU */
        for (volatile uint64_t s = (uint64_t)1u << retry; s; --s)
            __atomic_thread_fence(__ATOMIC_RELAXED);
        sched_yield();
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
