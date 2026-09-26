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
} irtcli_t;

/* 初始化。cap 为 0 时使用 IRTCLI_QUEUE_CAP_DEFAULT。返回 IRTCLI_OK 或负错误码 */
int irtcli_init(irtcli_t* c, const char* sock_path, uint32_t cap,
                irtcli_alert_fn alert, void* user_data);

/* 释放队列与连接 */
void irtcli_close(irtcli_t* c);

/* 建立/重连到 rtdbd。返回 IRTCLI_OK 或负错误码（失败时仍可异步入队） */
int irtcli_connect(irtcli_t* c);

/* 切换同步 / 异步模式（默认异步） */
void irtcli_set_async(irtcli_t* c, bool async);

/* 写入。同步返回 IRTCLI_OK；异步返回 IRTCLI_QUEUED；失败返回负错误码 */
int irtcli_write_bool(irtcli_t* c, uint32_t id, bool value, uint64_t source_ts_ns);
int irtcli_write_int32(irtcli_t* c, uint32_t id, int32_t value, uint64_t source_ts_ns);
int irtcli_write_double(irtcli_t* c, uint32_t id, double value, uint64_t source_ts_ns);

/* 将队列中的条目按序提交到服务端。
 * 返回实际提交条数；连接不可用时返回 0 且保留队列（不丢数据）。 */
int irtcli_flush(irtcli_t* c);

/* 当前队列长度 */
uint32_t irtcli_queue_count(const irtcli_t* c);

#ifdef __cplusplus
}
#endif

#endif /* IRTCLI_CLIENT_H_ */
