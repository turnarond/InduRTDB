/**
 * @file rtdbd_session.h
 * @brief rtdbd 传输无关会话 + 延迟动作队列（M1.1，doc 13 §8.3-3）
 *
 * 为什么需要（§8.3-3 / §9.1 表「§7.3 作废」）：
 * - 现有 `rtdbd_conn_t` 把**连接级状态**（partition / 订阅+RBE / CAPS 位 /
 *   快照态 / cred）与 **UDS 传输细节**（fd / outq 环形队列）强耦合在一个结构里，
 *   导致 VSOA 前端无法复用业务语义。doc 13 §9.1 已把旧「UDS 薄适配 =
 *   recv_all → dispatch → send_all」判为**作废**，要求先抽 `rtdbd_session_t`
 *   与延迟动作队列，UDS / VSOA 各做一层编解码适配，共享同一套业务语义。
 * - 延迟动作队列解决 SDK 约束「不能在回调里 reconnect」：reconnect / close /
 *   订阅重建 / 触发全量（on_join）/ peer 增删一律**入队**，由主循环**循环尾
 *   统一执行**（§8.3-3 题 3、§8.3-4 来源校验失败入队关闭、§8.3-6 peer 增删）。
 *
 * 传输无关约束（自律，守卫脚本尚未覆盖本目录）：
 * 本模块**禁止**出现 fd、socket、poll、select、vsoa 头或 vsoa 类型、
 * rtdbd_conn_t、outq_push、SO_PEERCRED 等传输/VSOA 符号；对外暴露
 * `rtdbd_session_sink_t`（sink 表）与 `rtdbd_notify_rbe_fn`（RBE 判定回调）
 * 两张 C 函数指针表，均不含传输/VSOA 类型（仅在 rtdbd 进程内有效，§8.3-3）。
 *
 * 零动态分配：延迟动作队列为定长环形（RTDBD_DEFER_QUEUE_CAP）；订阅表复用
 * 既有纯叶子模块 subtab（订阅成员 + RBE 状态合并表）。
 */
#ifndef RTDBD_SESSION_H_
#define RTDBD_SESSION_H_

#include <rtdbd/protocol.h> /* RTDBD_SUB_MAX 等协议常量 */
#include "subtab.h"          /* 订阅成员 + RBE 状态合并表（纯叶子模块） */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 会话角色（§8.3-4 鉴权：plc 业务写 / sync_peer 仅 /sync/ / sync_admin 管控）---- */
typedef enum {
    RTDBD_ROLE_UNKNOWN    = 0,
    RTDBD_ROLE_PLC        = 1, /* 本地 loopback 业务写 */
    RTDBD_ROLE_SYNC_PEER  = 2, /* 远端复制重放，仅允许 /sync/ 前缀 URL */
    RTDBD_ROLE_SYNC_ADMIN = 3  /* 成员表管控（/sync/peers/set|get） */
} rtdbd_session_role_t;

/* ---- 凭据（§8.3-4：握手绑定身份；按点鉴权仍复用 irt_policy_allows）---- */
typedef struct {
    uint32_t uid;
    uint32_t pid;
    bool     cred_ok;
} rtdbd_session_cred_t;

/* ---- 发送句柄：C 函数指针表，只在 rtdbd 进程内（§8.3-3）---- */
typedef struct {
    /* 发送一帧给对端；true = 已接受，false = 背压丢弃（调用方须计丢弃数）。 */
    bool (*send)(void *owner, const void *frame, size_t len);
    /* 关闭会话（transport 无关）。 */
    void (*close)(void *owner);
    /* 适配器私有上下文（UDS 连接槽 / VSOA cli 句柄等）。 */
    void *owner;
} rtdbd_session_sink_t;

/* ---- 传输无关会话（§8.3-3 字段清单）---- */
typedef struct {
    /* 连接级默认分区（UDS 由 OP_SELECT 设置，VSOA 由握手携带）。 */
    uint32_t     partition;
    /* 订阅**有序视图**（快照分页顺序）+ 计数；与下方 sub 合并表必须一致，
     * 一致性由 rtdbd_session_subs_consistent() 判定并有单测锁定。 */
    uint32_t     subs[RTDBD_SUB_MAX];
    uint32_t     nsubs;
    /* 订阅成员 + RBE 状态合并表。 */
    rtdbd_subtab_t sub;
    /* 能力位（RTDBD_CAPS_F_*）与协商位。 */
    uint32_t     caps_flags;
    bool         caps_negotiated;
    /* 快照流状态（每轮预算分页推进，§8.3-3）。 */
    bool         snap_active;
    uint32_t     snap_next;
    uint32_t     snap_emitted;
    /* VSOA 握手身份（§8.3-1）：service_id + 服务端分配的 epoch。
     * UDS 侧不参与纪元隔离，这两个字段恒为 0 / 空。 */
    char         service_id[64];
    uint64_t     epoch;
    rtdbd_session_role_t role;
    rtdbd_session_cred_t cred;
    /* 发送句柄。 */
    rtdbd_session_sink_t sink;
} rtdbd_session_t;

/* 会话初始化：清零传输无关状态并建立订阅表。initial_sub_cap 建议
 * RTDBD_SUBTAB_MIN_CAP；返回 false 表示订阅表初始分配失败。 */
bool rtdbd_session_init(rtdbd_session_t *s, uint32_t initial_sub_cap);

/* 复位（连接断开 / 槽位复用）：清零传输无关状态，保留订阅表容量。 */
void rtdbd_session_reset(rtdbd_session_t *s);

/* 释放订阅表内存（进程退出 / 槽位释放）。 */
void rtdbd_session_free(rtdbd_session_t *s);

/* 置 service_id（握手携带）；超长（含结尾 '\0' 溢出）返回 false 且不写入。 */
bool rtdbd_session_set_service_id(rtdbd_session_t *s, const char *id);

/* 是否为远端复制角色（仅允许 /sync/ 前缀 URL，不得订阅/收 NOTIFY，防回环）。 */
bool rtdbd_session_is_sync_peer(const rtdbd_session_t *s);

/* ---- 订阅集合（有序视图 + RBE 合并表，两份必须一致）---- */

/* 订阅新增结果三态：**幂等**语义须与既有 UDS 行为一致
 * （已订阅不是错误；仅超上限 / 表满才是失败）。 */
typedef enum {
    RTDBD_SUBS_ADD_OK     = 0, /* 新增成功 */
    RTDBD_SUBS_ADD_EXISTS = 1, /* 已是订阅（幂等） */
    RTDBD_SUBS_ADD_FAIL   = 2  /* 失败：超上限或合并表已满 */
} rtdbd_subs_add_result_t;

/* 新增订阅：同时写有序视图与 RBE 合并表（表满时不写视图，保持两份一致）。 */
rtdbd_subs_add_result_t rtdbd_session_subs_add(rtdbd_session_t *s, uint32_t point_id,
                                                uint32_t max_subs);

/* 退订：清 RBE 状态（重新订阅首值必发）+ 从有序视图移除（末位交换法，
 * 与既有 UDS 行为一致，快照分页顺序随之变化）。 */
void rtdbd_session_subs_remove(rtdbd_session_t *s, uint32_t point_id);

/* 原子替换整个订阅集：先在临时合并表构建完整集合，全部成功后再交换；
 * 任一步失败释放临时表，旧订阅集保持不变。 */
bool rtdbd_session_install_subs(rtdbd_session_t *s, const uint32_t *ids, uint32_t n);

/* 订阅数 / 按序取点 / 是否已订阅（成员判断走合并表）。 */
uint32_t rtdbd_session_subs_count(const rtdbd_session_t *s);
bool rtdbd_session_subs_get(const rtdbd_session_t *s, uint32_t idx, uint32_t *out_point_id);
bool rtdbd_session_subs_contains(const rtdbd_session_t *s, uint32_t point_id);

/* 两份视图一致性自检（有序视图每项都在合并表中，且计数相等）。 */
bool rtdbd_session_subs_consistent(const rtdbd_session_t *s);

/* ---- 延迟动作队列（§8.3-3）---- */

/* 动作类型：reconnect / close / 订阅重建 / 触发全量 / peer 增删。 */
typedef enum {
    RTDBD_DEFER_NONE              = 0,
    RTDBD_DEFER_RECONNECT         = 1,
    RTDBD_DEFER_CLOSE             = 2,
    RTDBD_DEFER_REBUILD_SUBS      = 3,
    RTDBD_DEFER_TRIGGER_FULLSYNC  = 4,
    RTDBD_DEFER_PEER_ADD          = 5,
    RTDBD_DEFER_PEER_DEL          = 6
} rtdbd_defer_kind_t;

typedef struct {
    rtdbd_defer_kind_t kind;
    uint32_t           session_id; /* 目标会话槽位（transport 无关句柄） */
    uint32_t           arg;        /* 动作参数（如 peer 索引） */
    uint64_t           seq;        /* 入队序号（诊断用） */
    /* A2：入队时目标会话的 generation。0 = 未启用校验（保留旧 API 兼容）。
     * drain 时与目标当前 generation 不符 → 目标已失效 → no-op（防槽位复用误动作）。 */
    uint32_t           target_generation;
} rtdbd_defer_action_t;

/* 定长环形容量（零动态分配；满则拒绝并计数，不静默丢弃）。 */
#define RTDBD_DEFER_QUEUE_CAP 256u

typedef struct {
    rtdbd_defer_action_t slots[RTDBD_DEFER_QUEUE_CAP];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
    uint64_t enqueued; /* 累计入队 */
    uint64_t dropped;  /* 队列满被拒（不静默丢） */
    uint64_t executed; /* 出队交给执行方的计数 */
} rtdbd_defer_queue_t;

/* 队列初始化：清零全部槽位与计数。 */
void rtdbd_defer_queue_init(rtdbd_defer_queue_t *q);

/* 入队（延迟动作，**不得**在回调里直接执行）。队列满返回 false 并计
 * dropped；RTDBD_DEFER_NONE 视为非法动作同样拒绝。 */
bool rtdbd_defer_push(rtdbd_defer_queue_t *q, rtdbd_defer_kind_t kind,
                      uint32_t session_id, uint32_t arg);

/* A2：带 generation 的入队（生产路径用）。generation=0 表示不校验。
 * 旧 rtdbd_defer_push 保留为兼容包装（等价于 generation=0）。 */
bool rtdbd_defer_push_gen(rtdbd_defer_queue_t *q, rtdbd_defer_kind_t kind,
                          uint32_t session_id, uint32_t arg,
                          uint32_t target_generation);

/* 出队（FIFO）：交给主循环循环尾执行；空队列返回 false。 */
bool rtdbd_defer_pop(rtdbd_defer_queue_t *q, rtdbd_defer_action_t *out);

/* 当前待执行动作数。 */
uint32_t rtdbd_defer_count(const rtdbd_defer_queue_t *q);

/* 动作类型名（诊断用）。 */
const char *rtdbd_defer_kind_name(rtdbd_defer_kind_t kind);

/* ---- v3.12 M3.3 切片4：NOTIFY 门控诊断计数（传输无关）----
 *
 * 只含 uint64_t 纯标量 + 函数指针回调，不含任何 fd/socket/poll/vsoa 类型。
 * 实例由各前端持有（VSOA 侧为 adapter 全局聚合），**不进 rtdbd_session_t**，
 * 故不改会话布局 / 共享内存布局。4 个计数是整个前端的投递诊断量（非 per-session
 * 属性），用于 IR 验收区分「RBE 抑制」与「未订阅」，并证伪「回环泄露」。 */
typedef struct {
    uint64_t rbe_gate_hits;            /* RBE 通过并定向发送 */
    uint64_t rbe_gate_misses;          /* RBE 死区/量程未越过被抑制 */
    /* SNAPSHOT_DONE 仅投 caps_negotiated 会话。**VSOA 生产路径未接线，当前恒 0；
     * 当前无生产调用点；属切片 5/IR1，见变更卡留白**。IR 验收/运维读取时勿误认为
     * 已实现生产定向（getter 是本切片的唯一读出口，务必注意该字段恒 0）。 */
    uint64_t snapshot_done_caps_only;
    uint64_t peer_notify_leak;         /* 非订阅者/sync_peer 被判可发（须恒 0） */
} rtdbd_notify_counters_t;

typedef enum {
    /* 空槽 / sync_peer / 未订阅；以及 rbe==NULL 契约违约（见下，不计数）。 */
    RTDBD_NOTIFY_GATE_SKIP = 0,
    RTDBD_NOTIFY_GATE_MISS,      /* RBE 抑制 */
    RTDBD_NOTIFY_GATE_HIT        /* 命中：应定向发送 */
} rtdbd_notify_gate_t;

/* RBE 判定回调：**仅在会话通过 SKIP 过滤后**才调用（保证未订阅 / sync_peer
 * 不消耗 RBE 状态 —— rtdbd_dispatch_notify_rbe 内部推进 subtab 状态，有副作用）。
 * 由前端包装 rtdbd_dispatch_notify_rbe 提供，本模块零 dispatch 类型依赖。 */
typedef bool (*rtdbd_notify_rbe_fn)(void *ctx, rtdbd_session_t *s, uint32_t point_id);

/* 门控纯函数：严格按「空会话 → sync_peer → 未订阅 → 才调 RBE」顺序，SKIP 类
 * 绝不触碰 RBE。ctr 只是诊断出口：NULL 时仍正常完成判定并允许发送，仅在 ++ 处判空。
 * **rbe 为 NULL 视为调用方契约违约，返回 SKIP 且不递增任何计数**（不得伪装成
 * 死区抑制）。 */
rtdbd_notify_gate_t rtdbd_session_notify_gate(rtdbd_session_t *s, uint32_t point_id,
    rtdbd_notify_rbe_fn rbe, void *rbe_ctx, rtdbd_notify_counters_t *ctr);

/* 发送点护栏：正常路径不可达（gate 已过滤非法会话），仅作防御 +
 * 使 peer_notify_leak（「实际发给非订阅者的次数」）计数出口可观测且正常恒 0；
 * 保留合理，不删除（评审 M-4）。ctr 为 NULL 时同样正常判定、仅 ++ 处判空。 */
bool rtdbd_session_notify_deliver_guard(const rtdbd_session_t *s, uint32_t point_id,
    rtdbd_notify_counters_t *ctr);

/* VSOA 生产路径未接线，当前恒 0；当前无生产调用点；属切片 5/IR1，见变更卡留白。
 * 供验收取证的纯逻辑 API，不由任何生产路径调用。 */
void rtdbd_notify_counters_record_snapshot_done_caps_only(rtdbd_notify_counters_t *ctr);
void rtdbd_notify_counters_snapshot(const rtdbd_notify_counters_t *ctr,
                                    rtdbd_notify_counters_t *out);
/* reset 为供验收取证的纯逻辑清零 API，当前无生产调用点（见变更卡留白）。 */
void rtdbd_notify_counters_reset(rtdbd_notify_counters_t *ctr);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_SESSION_H_ */
