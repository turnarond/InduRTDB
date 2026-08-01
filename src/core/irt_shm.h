/**
 * @file irt_shm.h
 * @brief 共享内存段管理 (直译自 v2.x SharedMemorySegment)
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_SHM_H_
#define IRT_CORE_IRT_SHM_H_

#include <osal/irt_osal.h>
#include <internal/irt_types.h>

/* 段名前缀 */
#define IRT_SHM_PREFIX  "/indurtdb_"

/* 总大小: header(64) + N*128(point) + M*16(subscriber) */
static inline size_t irt_shm_total_size(uint32_t max_points,
                                         uint32_t max_subscribers) {
    return sizeof(irt_header_t)
         + (size_t)max_points    * sizeof(indurtdb_point_t)
         + (size_t)max_subscribers * sizeof(irt_subscriber_entry_t);
}

typedef struct {
    irt_shm_os_t os;               /* OSAL 句柄 */
    void*        base;             /* mmap 基址 */
    size_t       total_size;
    uint32_t     max_points;
    uint32_t     max_subscribers;
} irt_shm_t;

/* 创建或 attach 共享内存段. 成功返回 0. owner 负责初始化 header */
int  irt_shm_init(irt_shm_t* s, const char* instance_id,
                  uint32_t max_points, uint32_t max_subscribers);

/* 释放共享内存段 */
void irt_shm_shutdown(irt_shm_t* s);

bool irt_shm_is_owner(const irt_shm_t* s);

/* 直接返回共享内存中的子区域指针 */
irt_header_t*           irt_shm_header(const irt_shm_t* s);
indurtdb_point_t*       irt_shm_points(const irt_shm_t* s);
irt_subscriber_entry_t* irt_shm_subscribers(const irt_shm_t* s);

#endif /* IRT_CORE_IRT_SHM_H_ */
