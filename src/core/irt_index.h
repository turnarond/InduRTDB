/**
 * @file irt_index.h
 * @brief 共享内存内 name→id 索引 (开放寻址 + 墓碑, 定长预分配, 零堆分配)
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 *
 * 设计依据: docs/03-设计文档/07-v3.4-布局v2与APIv2方案设计.md §3.2
 *   - 条目 8B: { uint32 hash; uint32 point_id; }
 *     point_id == IRT_INDEX_EMPTY      空槽
 *     point_id == IRT_INDEX_TOMBSTONE  墓碑(已删除, 探测链不可断)
 *   - 桶数 B = roundup_pow2(max_points * 2), 负载因子 ≤ 0.5, 位掩码取模
 *   - 哈希 FNV-1a 32 (纯整数运算, 无依赖)
 *   - 冲突比较复用点位区内 name[64], 索引不重复存字符串
 *
 * 并发约定:
 *   - insert / remove 走既有全局 seqlock 写锁 (注册期集中写, 非热路径);
 *   - lookup 走 seqlock 读重试 (不引入锁、不进内核);
 *   - 若调用方已持写锁, 用 *_locked() 变体, 避免嵌套 write_begin 自锁。
 */

#ifndef IRT_CORE_IRT_INDEX_H_
#define IRT_CORE_IRT_INDEX_H_

#include "core/irt_shm.h"

#define IRT_INDEX_EMPTY      UINT32_MAX
#define IRT_INDEX_TOMBSTONE  (UINT32_MAX - 1u)

/* 索引条目 (8B) */
typedef struct {
    uint32_t hash;       /* FNV-1a 32 of name */
    uint32_t point_id;   /* 点位 id, 或 IRT_INDEX_EMPTY / IRT_INDEX_TOMBSTONE */
} __attribute__((packed, aligned(8))) irt_index_slot_t;

IRT_STATIC_ASSERT(sizeof(irt_index_slot_t) == 8, "index slot must be 8 bytes");

/* 索引视图 (不持有内存, 仅指向段内索引区) */
typedef struct {
    irt_index_slot_t* slots;
    uint32_t          capacity;   /* 桶数 B, 2 的幂 */
    uint32_t          mask;       /* B - 1 */
} irt_index_t;

/* 桶数: roundup_pow2(max_points * 2), 下限 8 */
uint32_t irt_index_capacity_for(uint32_t max_points);

/* 绑定索引视图到段内索引区. 成功 0 */
int irt_index_bind(irt_index_t* ix, const irt_shm_t* s);

/* owner 建段时清空索引区 (全部置 EMPTY). 非 owner 不得调用 */
void irt_index_clear(irt_shm_t* s);

/* 注册 name → point_id. 同名已存在则更新为新的 point_id (幂等)。
 * 返回 0 / INDURTDB_ERR_ARG / INDURTDB_ERR_FULL */
int irt_index_insert(irt_shm_t* s, const char* name, uint32_t point_id);

/* 注销 name (置墓碑, 不断探测链). 返回 0 / INDURTDB_ERR_NOT_FOUND */
int irt_index_remove(irt_shm_t* s, const char* name);

/* 按名查找. 找到返回 0 并写 *out_id; 未找到 INDURTDB_ERR_NOT_FOUND;
 * 写冲突重试耗尽返回 INDURTDB_ERR_BUSY */
int irt_index_lookup(const irt_shm_t* s, const char* name, uint32_t* out_id);

/* 调用方已持写锁时的变体 (如 load_config 的临界区内) */
int irt_index_insert_locked(irt_shm_t* s, const char* name, uint32_t point_id);
int irt_index_remove_locked(irt_shm_t* s, const char* name);

/* FNV-1a 32 (暴露给用例构造同桶冲突) */
uint32_t irt_hash_fnv1a32(const char* s);

#endif /* IRT_CORE_IRT_INDEX_H_ */
