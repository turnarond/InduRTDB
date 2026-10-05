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
/* v3.3 及以前为 1；v3.4 布局 v2 起为 2。
 * 版本不匹配一律拒绝挂载，绝不按新布局解释旧段。 */
#define IRT_SHM_VERSION  2u

/* 共享内存头部 (v3.4 布局 v2: 128 字节)
 *
 * 兼容约束：前 44 字节（magic / version / max_points / max_subscribers /
 * write_seq / owner_pid / stats）与 v1 逐字节同偏移，旧字段语义不变。
 * v2 新增：crc32、flags、scan_skipped、四个区段偏移 + 保留区。
 *
 * 区段偏移写进 Header 而非按 sizeof 硬算：后续新增/调整区段时
 * 只需改布局计算，不再牵动所有访问点。
 */
typedef struct {
    uint32_t magic;                 /* 0   */
    uint32_t version;               /* 4   */
    uint32_t max_points;            /* 8   */
    uint32_t max_subscribers;       /* 12  */
    uint64_t write_seq;             /* 16  Seqlock 序列号 */
    int32_t  owner_pid;             /* 24  创建进程 PID (0=未知) */
    struct {
        uint64_t writes;            /* 28  */
        uint64_t timeouts;          /* 36  */
    } stats;
    uint32_t crc32;                 /* 44  Header 完整性校验（不含本字段） */
    uint32_t flags;                 /* 48  预留特性位 */
    uint32_t reserved_scan;         /* 52  保留：曾拟放 scan_skipped，因子在 CRC 范围[48..71]内、
                                          运行时自增会破坏 CRC 而拒掉合法段，故改为保留（恒 0） */
    uint32_t off_points;            /* 56  点位区段内偏移 */
    uint32_t off_index;             /* 60  name→id 索引区段内偏移（T2 启用） */
    uint32_t off_meta;              /* 64  元数据区段内偏移（T3 启用） */
    uint32_t off_subs;              /* 68  订阅者心跳区段内偏移 */
    uint32_t index_count;           /* 72  T2: 索引当前装载条目数（易变, 不入 CRC） */
    uint32_t scan_skipped;          /* 76  v3.4 T7: 超时扫描被跳过的点数（保留区, 不在 CRC 范围） */
    uint8_t  reserved[48];          /* 80..127 */
} __attribute__((packed, aligned(64))) irt_header_t;

IRT_STATIC_ASSERT(sizeof(irt_header_t) == 128, "v3.4 layout v2: header must be 128 bytes");

/* 前 44 字节与 v1 同偏移 —— 迁移工具与旧版读取代码可据此定位 */
IRT_STATIC_ASSERT(offsetof(irt_header_t, magic)            == 0,  "magic offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, version)          == 4,  "version offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, max_points)       == 8,  "max_points offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, max_subscribers)  == 12, "max_subscribers offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, write_seq)        == 16, "write_seq offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, owner_pid)        == 24, "owner_pid offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, stats)            == 28, "stats offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, crc32)            == 44, "crc32 offset");
IRT_STATIC_ASSERT(offsetof(irt_header_t, index_count)      == 72, "index_count offset");
/* scan_skipped 必须落在 CRC 范围 [48..71] 之外，否则运行时自增会使 attach 校验失败 */
IRT_STATIC_ASSERT(offsetof(irt_header_t, scan_skipped)      == 76, "scan_skipped must be outside CRC range [48..71]");

/* 区域对齐兜底（M-2）：header(128) 与 point(128) 均为 32 对齐；
 * 索引桶数 = roundup_pow2(2N)（2 的幂、≥8），故 index_size = cap*8 必为 32 的倍数；
 * 由此 meta 区(需 align 32) / subs 区(需 align 16) 相对 hdr 的偏移均自然对齐，
 * indurtdb_meta_t / irt_subscriber_entry_t 不会因未对齐产生 UB。 */
IRT_STATIC_ASSERT(sizeof(irt_header_t) % 32 == 0, "header must be 32-aligned so meta/subs regions inherit alignment");
IRT_STATIC_ASSERT(sizeof(indurtdb_point_t) % 32 == 0, "point must be 32-aligned so subsequent regions stay aligned");

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

/* v3.3: source_timestamp_ns 落在 padding 区（offset 112，8B 对齐），
 * sizeof 与既有字段偏移均不变 —— ABI 仍为 v1。 */
IRT_STATIC_ASSERT(offsetof(indurtdb_point_t, source_timestamp_ns) == 112,
                  "source_timestamp_ns must be at offset 112");
IRT_STATIC_ASSERT(offsetof(indurtdb_point_t, name) == 45,
                  "name offset must not change (ABI v1)");
IRT_STATIC_ASSERT(offsetof(indurtdb_point_t, quality) == 41,
                  "quality offset must not change (ABI v1)");

#endif /* IRT_INTERNAL_IRT_TYPES_H_ */
