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
#define RTDBD_PROTO_VERSION 2u

/* ---- 操作码 ---- */
#define RTDBD_OP_PING        1u
#define RTDBD_OP_WRITE       2u
#define RTDBD_OP_AUDIT_DUMP  3u
#define RTDBD_OP_SUBSCRIBE   4u
#define RTDBD_OP_UNSUBSCRIBE 5u
#define RTDBD_OP_NOTIFY      6u /* 服务端→客户端推送（非请求响应） */
/* v3.4 T9 新增：按名查找与元数据管控通道 */
#define RTDBD_OP_FIND_BY_NAME 7u /* 请求：name[64]；响应：point_id（status==OK 时） */
#define RTDBD_OP_GET_META     8u /* 请求：point_id；响应：32B meta（status==OK 时） */
#define RTDBD_OP_SET_META     9u /* 请求：point_id + 32B meta；管控写，须鉴权 */

/* 单连接最大订阅点数 */
#define RTDBD_SUB_MAX 32u

/* ---- 状态码 ---- */
#define RTDBD_ST_OK               0u
#define RTDBD_ST_BAD_REQUEST      1u
#define RTDBD_ST_DENIED           2u
#define RTDBD_ST_INTERNAL         3u
#define RTDBD_ST_NOT_FOUND        4u /* 按名未找到 / 越界 id */
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

/* 订阅/退订请求负载 4B */
typedef struct {
    uint32_t point_id;
} rtdbd_sub_req_t;

/* 变更通知负载 32B（服务端主动推送） */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  reserved[3];
    uint64_t value_bits;
    uint64_t timestamp_ns;   /* 入库时刻 */
    uint64_t source_ts_ns;   /* 采集时刻 */
} rtdbd_notify_t;

/* ---- v3.4 T9 新操作负载 ---- */

/* FIND_BY_NAME 请求负载 64B（与 indurtdb_point_t.name[64] 等长） */
typedef struct {
    char name[64];
} rtdbd_find_req_t;

/* FIND_BY_NAME 响应负载 4B（仅 status==OK 时携带） */
typedef struct {
    uint32_t point_id;
} rtdbd_find_resp_t;

/* 元数据负载 32B：与 indurtdb_meta_t 逐字段布局一致，服务端可直接 memcpy */
typedef struct {
    double   eur_min;    /* 0–7   : 工程量程下限 */
    double   eur_max;    /* 8–15  : 工程量程上限 */
    float    deadband;   /* 16–19 : 变化阈值（只存参数） */
    uint32_t flags;      /* 20–23 : 是否启用量程 / 死区等 */
    uint8_t  reserved[8];/* 24–31 : 保留（须为 0） */
} rtdbd_meta_payload_t;

/* GET_META 请求负载 4B */
typedef struct {
    uint32_t point_id;
} rtdbd_meta_req_t;

/* SET_META 请求负载 36B（point_id + 32B meta） */
typedef struct {
    uint32_t             point_id;
    rtdbd_meta_payload_t meta;
} rtdbd_set_meta_req_t;

#endif /* RTDBD_PROTOCOL_H_ */
