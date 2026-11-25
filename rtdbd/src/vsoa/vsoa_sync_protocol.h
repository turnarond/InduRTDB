/**
 * @file vsoa_sync_protocol.h
 * @brief S2 同步通道专有 POD（VSOA URL 层，不进 UDS opcode/protocol.h）。
 *
 * 13 号 D1-D5：全对等 active-active、纪元隔离、VSOA 传输、异步+WAIT。
 * 所有 POD 定长、零序列化、memcpy 传递；RTDBD_STATIC_ASSERT 锁尺寸。
 * 本头无 VSOA SDK 依赖（unit 可直测），仅依赖 protocol.h 的常量/宏。
 */
#ifndef RTDBD_VSOA_SYNC_PROTOCOL_H_
#define RTDBD_VSOA_SYNC_PROTOCOL_H_

#include <stdint.h>
#include <stddef.h>
#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>

#ifdef __cplusplus
extern "C" {
#endif

/* S2 页大小上限：受 adapter resp[8192] 约束（vsoa_adapter.c:38），
 * 偏离 13 号:193 的 128KB/160 帧——S2b 扩缓冲后再对齐。 */
#define RTDBD_SYNC_RESP_MAX      8192u
#define RTDBD_SYNC_WAIT_MAX      16u
#define RTDBD_SYNC_WAIT_TIMEOUT_MS 3000u

/* hello：版本 1，交换 epoch + S1 digest 数值字段 + node_id（不含 hex）。 */
typedef struct {
    uint64_t epoch;
    uint64_t struct_hash;
    uint64_t value_digest;
    uint32_t n_points;
    char     node_id[RTDBD_PEER_NODE_ID_MAX];
} rtdbd_sync_hello_req_t;

typedef struct {
    uint64_t epoch;
    uint64_t struct_hash;
    uint64_t value_digest;
    uint32_t n_points;
    char     node_id[RTDBD_PEER_NODE_ID_MAX];
    uint8_t  accepted;     /* S2 恒 1；全量源选择留 S3 */
    uint8_t  reserved[7];
} rtdbd_sync_hello_resp_t;

/* delta：13 号:246 sync_rec 的 VSOA 专有形态；applied_offset = 扫描游标水位
 * （S2 无 wal，不宣称真实增量正确性——留 S3）。 */
typedef struct {
    uint64_t from_offset;
    uint32_t max_records;
} rtdbd_sync_delta_req_t;

typedef struct {
    uint32_t point_id;
    uint64_t value_bits;
    uint8_t  quality;
    uint8_t  type;
    uint8_t  reserved0[6];
    uint64_t source_timestamp_ns;
    uint64_t epoch;
    char     node_id[RTDBD_PEER_NODE_ID_MAX];
    uint64_t boot_id;
    uint64_t offset;
} rtdbd_sync_delta_rec_t;

typedef struct {
    uint64_t applied_offset; /* 最后扫描 id+1（不重不漏可证伪） */
    uint32_t n_records;
    uint8_t  has_more;
    uint8_t  reserved[3];
} rtdbd_sync_delta_resp_t;

/* snapshot：header 走 vsoa_payload.param、点位数组走 .data（四段 SDK）。 */
typedef struct {
    uint32_t page_index;
    uint32_t page_size;
} rtdbd_sync_snapshot_req_t;

typedef struct {
    uint32_t page_index;
    uint32_t total_pages;
    uint32_t n_points;
    uint32_t reserved;
} rtdbd_sync_snapshot_resp_t;

/* WAIT：D5 异步 + 可选 N-1 ack、超时不回滚。 */
typedef struct {
    uint64_t min_epoch;
    uint32_t required_acks;
    uint32_t timeout_ms;
} rtdbd_sync_wait_req_t;

typedef struct {
    uint32_t acks;
    uint8_t  ok;
    uint8_t  reserved[3];
} rtdbd_sync_wait_resp_t;

/* 单页可容纳的点位数（连续 buffer 逐条 memcpy，不用 aligned flexible-array：
 * 避免 header 后 pad 溢出 resp[8192]，终验 P1-4）。 */
static inline uint32_t rtdbd_sync_page_capacity(uint32_t requested)
{
    const uint32_t cap = (RTDBD_SYNC_RESP_MAX - (uint32_t)sizeof(indurtdb_point_t)) /
                         (uint32_t)sizeof(indurtdb_point_t);
    return requested < cap ? requested : cap;
}

/* 定长断言（防止跨端布局漂移）。 */
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_hello_req_t) == 64u, "sync hello req layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_hello_resp_t) == 72u, "sync hello resp layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_delta_req_t) == 16u, "sync delta req layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_delta_rec_t) == 88u, "sync delta rec layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_delta_resp_t) == 16u, "sync delta resp layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_snapshot_req_t) == 8u, "sync snapshot req layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_snapshot_resp_t) == 16u, "sync snapshot resp layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_wait_req_t) == 16u, "sync wait req layout");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_sync_wait_resp_t) == 8u, "sync wait resp layout");
RTDBD_STATIC_ASSERT(sizeof(indurtdb_point_t) == 128u, "sync snapshot point layout");

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_VSOA_SYNC_PROTOCOL_H_ */
