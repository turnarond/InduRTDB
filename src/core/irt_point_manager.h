/**
 * @file irt_point_manager.h
 * @brief 点位管理器 (直译自 v2.x PointManager)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_POINT_MANAGER_H_
#define IRT_CORE_IRT_POINT_MANAGER_H_

#include "core/irt_shm.h"

typedef struct {
    irt_shm_t* shm;
    uint32_t   max_points;
} irt_pm_t;

/* 绑定到已初始化的共享内存段 */
void irt_pm_init(irt_pm_t* pm, irt_shm_t* shm);

/* 写入 (成功 0, id 无效 -1, 写冲突 -2, 只读点位 -3) */
int irt_pm_write_bool(irt_pm_t* pm, uint32_t id, bool value);
int irt_pm_write_int32(irt_pm_t* pm, uint32_t id, int32_t value);
int irt_pm_write_double(irt_pm_t* pm, uint32_t id, double value);
int irt_pm_write_string(irt_pm_t* pm, uint32_t id, const char* value);

/* 读取 (seqlock 保护, 拷贝到 out, 成功 0) */
int irt_pm_read(irt_pm_t* pm, uint32_t id, indurtdb_point_t* out);

/* 单拷贝 peek (seqlock 保护, 返回指向线程本地缓冲的指针).
 * 与 read() 的区别: 无需调用方提供 out 缓冲, 但返回值在下次 peek() 时被覆盖. */
const indurtdb_point_t* irt_pm_peek(irt_pm_t* pm, uint32_t id);

/* 校验与统计 */
bool     irt_pm_validate_id(const irt_pm_t* pm, uint32_t id);
uint64_t irt_pm_write_count(const irt_pm_t* pm);

/* 超时检测: 扫描所有点位, 将超过 timeout_ns 未更新的点位标记为 QUALITY_TIMEOUT.
 * 返回本次检测到的超时点数. timeout_ns=0 则跳过. */
int irt_pm_check_timeouts(irt_pm_t* pm, uint64_t timeout_ns);

#endif /* IRT_CORE_IRT_POINT_MANAGER_H_ */
