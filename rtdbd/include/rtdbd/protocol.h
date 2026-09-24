/**
 * @file protocol.h
 * @brief rtdbd 极简二进制协议（与 shm 点位结构对齐，几乎零序列化开销）
 *
 * 设计约束：
 * - 不引入 protobuf 等第三方依赖
 * - 协议头带 magic + version，版本不匹配立即拒绝（不静默）
 * - 定长结构，便于零拷贝/最小拷贝
 */
#ifndef RTDBD_PROTOCOL_H_
#define RTDBD_PROTOCOL_H_

#include <stdint.h>

#define RTDBD_MAGIC        0x52424431u /* "RBD1" */
#define RTDBD_PROTO_VERSION 1u

/* ---- 操作码 ---- */
#define RTDBD_OP_PING        1u
#define RTDBD_OP_WRITE       2u
#define RTDBD_OP_AUDIT_DUMP  3u

/* ---- 状态码 ---- */
#define RTDBD_ST_OK               0u
#define RTDBD_ST_BAD_REQUEST      1u
#define RTDBD_ST_DENIED           2u
#define RTDBD_ST_INTERNAL         3u
#define RTDBD_ST_NOT_IMPLEMENTED  9u

/* ---- 点位类型（与 indurtdb.h 保持一致） ---- */
#define RTDBD_TYPE_BOOL   0u
#define RTDBD_TYPE_INT32  1u
#define RTDBD_TYPE_DOUBLE 2u

#define RTDBD_AUDIT_CAPACITY 256u

/* 请求头 12B */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t opcode;
    uint32_t payload_len;
} rtdbd_req_hdr_t;

/* 响应头 12B */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t status;
    uint32_t payload_len;
} rtdbd_resp_hdr_t;

/* 写请求负载 24B */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  reserved[3];
    uint64_t value_bits;    /* int32/double 的位模式；bool 用最低位 */
    uint64_t source_ts_ns;  /* T5 落地后写入 source_timestamp_ns；当前保留 */
} rtdbd_write_req_t;

/* 审计条目 20B（结构体按 4B 对齐） */
typedef struct {
    uint32_t pid;
    uint32_t uid;
    uint32_t point_id;
    uint64_t ts_ns;
} rtdbd_audit_entry_t;

#endif /* RTDBD_PROTOCOL_H_ */
