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

/* 跨语言静态断言：C 用 _Static_assert 关键字，C++ 用 static_assert 关键字 */
#if defined(__cplusplus)
#define RTDBD_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define RTDBD_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

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
/* v3.5 监控只读通道（读免鉴权，同 FIND_BY_NAME / GET_META） */
#define RTDBD_OP_GET        10u /* 请求：point_id；响应：64B 点位值快照（status==OK 时） */
#define RTDBD_OP_LIST       11u /* 请求：max+offset；响应：点位信息数组（id/type/access/name） */

/* v3.6 管控写通道：点位 CRUD（须鉴权，deny by default，同 SET_META/WRITE）。
 * 运行时在共享内存注册表内增/删/改名；重命名/删除同步维护 name→id 索引。
 * 仅新增 opcode，不改动既有负载布局，故协议主版本仍为 2。 */
#define RTDBD_OP_CREATE_POINT 12u /* 请求：id+type+access+name[64]；管控写，须鉴权 */
#define RTDBD_OP_DELETE_POINT 13u /* 请求：point_id；管控写，须鉴权 */
#define RTDBD_OP_RENAME_POINT 14u /* 请求：point_id+name[64]；管控写，须鉴权 */

/* v3.6 运行日志（只读。与 AUDIT_DUMP 同类：无点位维度，故不按 point 鉴权）。
 * 仅含进程运行事件，不含点位值。 */
#define RTDBD_OP_GET_LOG      15u /* 请求：max；响应：日志条目数组（最旧→最新） */

/* v3.7 主题B B1 健康快照（只读、免鉴权，同 GET_LOG 定位：仅运维观测，无点位值）。
 * 仅新增 opcode，不改动既有负载布局，故协议主版本仍为 2（v3.6 monitor 不发该
 * opcode，行为完全不变）。 */
#define RTDBD_OP_HEALTH      16u /* 请求：无负载；响应：rtdbd_health_t 快照 */

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
#define RTDBD_TYPE_STRING 3u
#define RTDBD_TYPE_INT64  4u
#define RTDBD_TYPE_UINT32 5u
#define RTDBD_TYPE_FLOAT  6u

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

/* ---- v3.5 监控只读通道：GET / LIST ---- */

/* GET 请求负载 4B */
typedef struct {
    uint32_t point_id;
} rtdbd_get_req_t;

/* GET 响应负载 64B：点位当前值快照（无锁读，与 indurtdb_point_t 布局对齐） */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  quality;
    uint8_t  reserved[2];
    uint64_t value_bits;    /* 数值类型的位模式（bool/int32/int64/uint32/float/double） */
    char     value_str[32]; /* 字符串类型的值（仅 TYPE_STRING 有意义） */
    uint64_t timestamp_ns;
    uint64_t source_ts_ns;
} rtdbd_get_resp_t;

/* LIST 请求负载 8B */
typedef struct {
    uint32_t max;    /* 单包最大条数，0 = 全部 */
    uint32_t offset; /* 起始 id（分页） */
} rtdbd_list_req_t;

/* LIST 单条点位信息 72B */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  access;
    uint8_t  reserved[2];
    char     name[64];
} rtdbd_point_info_t;

/* 布局锁死：与 rtdbd.c 的编解码、Python 端复刻保持一致 */
RTDBD_STATIC_ASSERT(sizeof(rtdbd_get_resp_t) == 64, "rtdbd_get_resp_t must be 64B");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_point_info_t) == 72, "rtdbd_point_info_t must be 72B");

/* ---- v3.6 管控写通道：点位 CRUD 负载 ---- */

/* CREATE_POINT 请求负载 72B（id + 类型 + 权限 + 名称） */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  access;
    uint8_t  reserved[2];
    char     name[64];
} rtdbd_create_req_t;

/* DELETE_POINT 请求负载 4B */
typedef struct {
    uint32_t point_id;
} rtdbd_delete_req_t;

/* RENAME_POINT 请求负载 68B（id + 新名称） */
typedef struct {
    uint32_t point_id;
    char     name[64];
} rtdbd_rename_req_t;

RTDBD_STATIC_ASSERT(sizeof(rtdbd_create_req_t) == 72, "rtdbd_create_req_t must be 72B");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_delete_req_t) == 4, "rtdbd_delete_req_t must be 4B");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_rename_req_t) == 68, "rtdbd_rename_req_t must be 68B");

/* ---- v3.6 运行日志 ---- */

/* 单条日志消息上限（含结尾 '\0'） */
#define RTDBD_LOG_ENTRY_MSG_MAX 116u

/* 日志级别 */
#define RTDBD_LOG_INFO  0u
#define RTDBD_LOG_WARN  1u
#define RTDBD_LOG_ERROR 2u

/* 服务端环形缓冲容量（监控台最多可见条数） */
#define RTDBD_LOG_CAPACITY 128u

/* GET_LOG 请求负载 4B */
typedef struct {
    uint32_t max;   /* 最多返回条数，0 = 全部（上限 RTDBD_LOG_CAPACITY） */
} rtdbd_log_req_t;

/* GET_LOG 单条日志 128B */
typedef struct {
    uint64_t ts_ns;                          /* 墙上时钟(CLOCK_REALTIME) 纳秒 */
    uint8_t  level;                          /* RTDBD_LOG_* */
    uint8_t  reserved[3];
    char     msg[RTDBD_LOG_ENTRY_MSG_MAX];
} rtdbd_log_entry_t;

RTDBD_STATIC_ASSERT(sizeof(rtdbd_log_req_t) == 4, "rtdbd_log_req_t must be 4B");
RTDBD_STATIC_ASSERT(sizeof(rtdbd_log_entry_t) == 128, "rtdbd_log_entry_t must be 128B");

/* ---- v3.7 主题B B1：健康快照 ---- */

/* 健康状态（与核心 indurtdb.h 的 INDURTDB_HEALTH_* 数值一致） */
#define RTDBD_HEALTH_OK        0
#define RTDBD_HEALTH_DEGRADED  1
#define RTDBD_HEALTH_UNHEALTHY 2

/* HEALTH 响应负载 72B（在 rtdbd_stats_t 基础上补充 n_conns；无请求负载） */
typedef struct {
    uint64_t n_writes;          /* 成功写次数 */
    uint64_t n_notifies;        /* 成功发出的 NOTIFY 数 */
    uint64_t decode_fail;       /* 请求解析失败 */
    uint64_t write_rejected;    /* 鉴权拒绝 */
    uint64_t write_error;       /* 写入返回非 0（非类型不支持） */
    uint64_t notify_drop;       /* 背压丢弃 */
    uint64_t notify_send_fail;  /* 出站发送失败 */
    uint64_t uptime_ns;         /* 进程启动至今纳秒 */
    uint32_t health;            /* RTDBD_HEALTH_* */
    uint32_t n_conns;           /* 当前连接数 */
} rtdbd_health_t;

RTDBD_STATIC_ASSERT(sizeof(rtdbd_health_t) == 72, "rtdbd_health_t must be 72B");

#endif /* RTDBD_PROTOCOL_H_ */
