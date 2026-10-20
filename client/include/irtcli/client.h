/**
 * @file client.h
 * @brief indurtdb-client —— rtdbd 的 RPC 客户端（含本地写队列）
 *
 * 定位：与核心库 `libindurtdb.a`（纯本地 shm）**并列但独立**。
 * 核心库不感知网络/RPC；容器进程或需要受控写入的进程使用本客户端。
 *
 * 两种写入模式（见设计文档 §4.4）：
 *   - 异步（默认）：仅入队即返回 IRTCLI_QUEUED，应用可见延迟为 ns 级，
 *                  不受 UDS RTT 影响。需周期性调用 irtcli_flush() 提交。
 *   - 同步：        直连服务端等确认，返回 IRTCLI_OK；延迟为一次 UDS 往返。
 *
 * 队列语义：
 *   - 同点位合并去重（只保留最新值），避免积压时重复提交过期值
 *   - 队列满触发告警回调，绝不静默丢弃
 *   - 重放时携带原始 source_ts_ns，保证采集时刻不失真
 */
#ifndef IRTCLI_CLIENT_H_
#define IRTCLI_CLIENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <indurtdb/indurtdb.h>

/* 跨语言静态断言：C 用 _Static_assert（C11），C++ 用 static_assert —— 与
 * rtdbd/protocol.h 的 RTDBD_STATIC_ASSERT 同一约定，保证头文件可被 C++ 测试包含。 */
#if defined(__cplusplus)
#define IRTCLI_STATIC_ASSERT(cond, msg) static_assert(cond, msg)
#else
#define IRTCLI_STATIC_ASSERT(cond, msg) _Static_assert(cond, msg)
#endif

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 返回码 ---- */
#define IRTCLI_OK          0  /* 同步写：已提交并确认 */
#define IRTCLI_QUEUED      1  /* 异步写：已入队，尚未提交 */
#define IRTCLI_ERR_ARG    -1
#define IRTCLI_ERR_FULL   -2  /* 队列已满（同时触发告警） */
#define IRTCLI_ERR_IO     -3  /* 连接/收发失败 */
#define IRTCLI_ERR_DENIED -4  /* 服务端拒绝（鉴权失败） */
#define IRTCLI_ERR_PROTO  -5  /* 协议错误/版本不匹配 */
#define IRTCLI_ERR_NOT_FOUND -6 /* 按名未找到 / 越界 id */

#define IRTCLI_QUEUE_CAP_DEFAULT 256u
#define IRTCLI_QUEUE_CAP_MAX     4096u
#define IRTCLI_SOCK_PATH_MAX     108u

/* ---- 点位类型（与 rtdbd 协议一致） ---- */
#define IRTCLI_TYPE_BOOL   0u
#define IRTCLI_TYPE_INT32  1u
#define IRTCLI_TYPE_DOUBLE 2u

/* ---- 告警回调：队列满、鉴权拒绝等必须可观测 ---- */
typedef void (*irtcli_alert_fn)(const char* msg, void* user_data);

/* ---- 队列条目 ---- */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  reserved[3];
    uint64_t value_bits;    /* int32/double 的位模式；bool 用最低位 */
    uint64_t source_ts_ns;  /* 原始采集时刻，重放时不得改写 */
} irtcli_entry_t;

/* ---- 客户端上下文 ---- */
typedef struct {
    int      fd;
    bool     connected;
    char     sock_path[IRTCLI_SOCK_PATH_MAX];

    irtcli_entry_t* queue;
    uint32_t        cap;
    uint32_t        head;
    uint32_t        count;

    bool            async;
    irtcli_alert_fn alert;
    void*           user_data;

    uint32_t partition;  /* v3.9 T6：本连接当前默认分区（irtcli_select 更新，初值 0） */
} irtcli_t;

/* ---- v3.9 T6：LIST 单条点位信息 ----
 * 与 rtdbd 协议 rtdbd_point_info_t **逐字段布局一致（76B）**，但刻意在公共头里
 * 重新声明：client.h 仅依赖 indurtdb.h，不得把 rtdbd/protocol.h 泄漏给集成方。 */
typedef struct {
    uint32_t point_id;
    uint8_t  type;
    uint8_t  access;
    uint8_t  reserved[2];
    char     name[64];
    uint32_t partition;
} irtcli_point_info_t;

IRTCLI_STATIC_ASSERT(sizeof(irtcli_point_info_t) == 76,
                     "irtcli_point_info_t must be 76B (与 rtdbd_point_info_t 一致)");

/* 初始化。cap 为 0 时使用 IRTCLI_QUEUE_CAP_DEFAULT。返回 IRTCLI_OK 或负错误码 */
int irtcli_init(irtcli_t* c, const char* sock_path, uint32_t cap,
                irtcli_alert_fn alert, void* user_data);

/* 释放队列与连接 */
void irtcli_close(irtcli_t* c);

/* 建立/重连到 rtdbd。返回 IRTCLI_OK 或负错误码（失败时仍可异步入队） */
int irtcli_connect(irtcli_t* c);

/* 切换同步 / 异步模式（默认异步） */
void irtcli_set_async(irtcli_t* c, bool async);

/* ---- v3.9 T6：连接级默认分区选择（OP_SELECT，Redis SELECT 语义） ----
 * 成功时把 c->partition 更新为 partition，后续按名查找/列举默认使用该分区；
 * 失败**不更新**连接状态（fail-closed）。返回 status_to_cli(status)。 */
int irtcli_select(irtcli_t* c, uint32_t partition);

/* 写入。同步返回 IRTCLI_OK；异步返回 IRTCLI_QUEUED；失败返回负错误码 */
int irtcli_write_bool(irtcli_t* c, uint32_t id, bool value, uint64_t source_ts_ns);
int irtcli_write_int32(irtcli_t* c, uint32_t id, int32_t value, uint64_t source_ts_ns);
int irtcli_write_double(irtcli_t* c, uint32_t id, double value, uint64_t source_ts_ns);

/* 将队列中的条目按序提交到服务端。
 * 返回实际提交条数；连接不可用时返回 0 且保留队列（不丢数据）。 */
int irtcli_flush(irtcli_t* c);

/* 当前队列长度 */
uint32_t irtcli_queue_count(const irtcli_t* c);

/* ---- v3.4 T9：协议 v2 管控通道（同步请求-响应） ---- */

/* 按名查找点位 id（走 rtdbd 共享索引）。
 * v3.9 T6：按**本连接当前默认分区**（c->partition，初值 0）分辨，等价于
 * irtcli_find_by_name_in(c, c->partition, name, out_id)。默认分区 0 时线格式
 * 与旧版逐字节一致。
 * 返回 IRTCLI_OK 且 *out_id 填充；未找到返回 IRTCLI_ERR_NOT_FOUND；
 * 鉴权失败返回 IRTCLI_ERR_DENIED；协议/IO 错误返回对应负码。 */
int irtcli_find_by_name(irtcli_t* c, const char* name, uint32_t* out_id);

/* v3.9 T6：在**指定分区**内按名查找点位 id，不依赖连接默认分区。
 * 语义与 irtcli_find_by_name 相同，仅分区来源不同（显式形参）。 */
int irtcli_find_by_name_in(irtcli_t* c, uint32_t partition, const char* name,
                           uint32_t* out_id);

/* v3.9 T6：列出**指定分区**的点位（OP_LIST）。
 * offset 为起始 id（分页），max 为单包最大条数上限（>0）。
 * out 为调用方缓冲（cap 条 irtcli_point_info_t）；n = 响应条数，
 * 若 n > cap 返回 IRTCLI_ERR_ARG（不截断、不越界），此时 *out_count = 0。
 * 成功返回 IRTCLI_OK 且 *out_count = n；失败返回 status_to_cli(status)（*out_count=0）。 */
int irtcli_list_in(irtcli_t* c, uint32_t partition, uint32_t offset, uint32_t max,
                   irtcli_point_info_t* out, uint32_t cap, uint32_t* out_count);

/* 读取点位元数据（32B，与 indurtdb_meta_t 一致）。
 * 返回 IRTCLI_OK 且 *out 填充；越界/未注册返回 IRTCLI_ERR_NOT_FOUND。 */
int irtcli_get_meta(irtcli_t* c, uint32_t id, indurtdb_meta_t* out);

/* 写入点位元数据（管控操作，服务端须鉴权）。
 * 返回 IRTCLI_OK 表示已落库；鉴权失败返回 IRTCLI_ERR_DENIED；
 * 越界/未注册返回 IRTCLI_ERR_NOT_FOUND。 */
int irtcli_set_meta(irtcli_t* c, uint32_t id, const indurtdb_meta_t* m);

#ifdef __cplusplus
}
#endif

#endif /* IRTCLI_CLIENT_H_ */
