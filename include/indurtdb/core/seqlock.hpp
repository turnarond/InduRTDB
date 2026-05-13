/**
 * @file seqlock.hpp
 * @brief Seqlock 轻量自由函数 —— 操作共享内存 Header.write_seq
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 *
 * 三个自由函数，零类层次，零异常，零堆分配。
 * 适用于工业实时数据库"多读少写"场景。
 *
 * 用法：
 *   uint64_t s0 = seqlock_write_begin(&header->write_seq);
 *   if (s0 & 1) return false;  // 写冲突
 *   // ... 更新数据 ...
 *   seqlock_write_end(&header->write_seq, s0);
 */

#pragma once

#include "../types/memory_layout.hpp"
#include <cstring>

namespace indurtdb {
namespace core {

/**
 * @brief 开始写入 —— 将序列号从偶数改为奇数
 * @param seq 指向 Header.write_seq 的指针
 * @return 当前序列号（偶数=获取成功，奇数=有并发写入）
 */
inline uint64_t seqlock_write_begin(uint64_t* seq) {
    uint64_t s = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    if (s & 1ULL) return s;                          // 奇数 = 写冲突
    __atomic_store_n(seq, s + 1, __ATOMIC_RELEASE);   // 标记写入中
    return s;
}

/**
 * @brief 完成写入 —— 恢复序列号为偶数
 * @param seq  指向 Header.write_seq 的指针
 * @param seq0 seqlock_write_begin 返回的序列号
 */
inline void seqlock_write_end(uint64_t* seq, uint64_t seq0) {
    __atomic_store_n(seq, seq0 + 2, __ATOMIC_RELEASE);
}

/**
 * @brief 无锁读取 —— 返回共享内存中一致的点位数据指针
 * @param seq    指向 Header.write_seq 的指针
 * @param points 共享内存点位数组基址
 * @param id     点位 ID
 * @return 点位数据指针（零拷贝），调用方不应长期持有
 */
inline const PointData* seqlock_read(const uint64_t* seq,
                                      const PointData* points,
                                      uint32_t id) {
    uint64_t s0, s1;
    s0 = 0; s1 = 1;  // 初始化不同，确保至少循环一次
    do {
        s0 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
        if (s0 & 1ULL) continue;                     // 写中，重试
        __atomic_thread_fence(__ATOMIC_ACQUIRE);
        // 此时可安全读取 points[id] —— 数据在共享内存中
        s1 = __atomic_load_n(seq, __ATOMIC_ACQUIRE);
    } while (s0 != s1);
    return &points[id];
}

} // namespace core
} // namespace indurtdb
