/**
 * @file irt_shm.h
 * @brief 共享内存段管理 (直译自 v2.x SharedMemorySegment)
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_CORE_IRT_SHM_H_
#define IRT_CORE_IRT_SHM_H_

#include <osal/irt_osal.h>
#include <internal/irt_types.h>

/* 段名前缀 */
#define IRT_SHM_PREFIX  "/indurtdb_"

/* irt_shm_init 错误码（负值）：区分"参数/通用失败"与"布局不兼容"，
 * 使调用方能明确提示"需迁移"而非笼统失败。 */
#define IRT_SHM_ERR_ARG     (-1)   /* 参数非法 / 容量不足 */
#define IRT_SHM_ERR_VERSION (-2)   /* 段版本与布局不兼容，拒绝挂载 */
#define IRT_SHM_ERR_CRC     (-3)   /* Header 完整性校验失败 */

/* ---- 布局 v2：区段大小与偏移集中计算 ----
 * 索引区（T2）与元数据区（T3）在 v3.4 后续任务启用；当前为 0。
 * 所有访问点一律走这里的偏移/Header 中的 off_*，禁止按 sizeof 硬算。 */

static inline size_t irt_layout_index_size(uint32_t max_points) {
    (void)max_points;
    return 0u;   /* v3.4 T2 起启用 */
}

static inline size_t irt_layout_meta_size(uint32_t max_points) {
    (void)max_points;
    return 0u;   /* v3.4 T3 起启用 */
}

static inline uint32_t irt_layout_off_points(void) {
    return (uint32_t)sizeof(irt_header_t);
}

static inline uint32_t irt_layout_off_index(uint32_t max_points) {
    return irt_layout_off_points()
         + (uint32_t)((size_t)max_points * sizeof(indurtdb_point_t));
}

static inline uint32_t irt_layout_off_meta(uint32_t max_points) {
    return irt_layout_off_index(max_points)
         + (uint32_t)irt_layout_index_size(max_points);
}

static inline uint32_t irt_layout_off_subs(uint32_t max_points) {
    return irt_layout_off_meta(max_points)
         + (uint32_t)irt_layout_meta_size(max_points);
}

/* 总大小: header(128) + N*128(point) + index + meta + M*16(subscriber) */
static inline size_t irt_shm_total_size(uint32_t max_points,
                                         uint32_t max_subscribers) {
    return sizeof(irt_header_t)
         + (size_t)max_points    * sizeof(indurtdb_point_t)
         + irt_layout_index_size(max_points)
         + irt_layout_meta_size(max_points)
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

/* Header 完整性：计算并写入 CRC（seal）/ 校验 magic+CRC（verify）。
 * verify 返回 1 表示完整，0 表示被篡改或 magic 不符。
 * CRC 覆盖整个 Header，但跳过 crc32 字段自身。 */
int irt_header_seal(irt_header_t* h);
int irt_header_verify(const irt_header_t* h);

/* 直接返回共享内存中的子区域指针 */
irt_header_t*           irt_shm_header(const irt_shm_t* s);
indurtdb_point_t*       irt_shm_points(const irt_shm_t* s);
irt_subscriber_entry_t* irt_shm_subscribers(const irt_shm_t* s);

#endif /* IRT_CORE_IRT_SHM_H_ */
