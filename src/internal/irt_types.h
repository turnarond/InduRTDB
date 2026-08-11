/**
 * @file irt_types.h
 * @brief 内部共享内存布局 (与 v2.x memory_layout.hpp 逐字节一致)
 * @version 3.1.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#ifndef IRT_INTERNAL_IRT_TYPES_H_
#define IRT_INTERNAL_IRT_TYPES_H_

#include <indurtdb/indurtdb.h>

#ifdef __cplusplus
#define IRT_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define IRT_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#define IRT_MAGIC        0x1DBA1DBAu
#define IRT_SHM_VERSION  1u

/* 共享内存头部 (64 字节, == v2.x InduRTDBHeader)
 * v3.1: 在填充区添加 owner_pid 用于崩溃恢复; 保持 64 字节不变 */
typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t max_points;
    uint32_t max_subscribers;
    uint64_t write_seq;          /* Seqlock 序列号 */
    int32_t  owner_pid;          /* 创建进程 PID (0=未知, 填充区复用) */
    struct {
        uint64_t writes;
        uint64_t timeouts;
    } stats;
} __attribute__((packed, aligned(64))) irt_header_t;

IRT_STATIC_ASSERT(sizeof(irt_header_t) == 64, "header must be 64 bytes");

/* 订阅者心跳条目 (16 字节, == v2.x SubscriberEntry) */
typedef struct {
    int32_t  pid;
    uint64_t last_heartbeat_ns;
    uint8_t  padding[4];
} __attribute__((packed, aligned(16))) irt_subscriber_entry_t;

IRT_STATIC_ASSERT(sizeof(irt_subscriber_entry_t) == 16,
                  "subscriber entry must be 16 bytes");

IRT_STATIC_ASSERT(sizeof(indurtdb_point_t) == 128,
                  "point must be 128 bytes");

#endif /* IRT_INTERNAL_IRT_TYPES_H_ */
