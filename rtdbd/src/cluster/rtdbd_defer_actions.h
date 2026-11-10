/**
 * @file rtdbd_defer_actions.h
 * @brief v3.12 M3.2 ③：延迟动作的**循环尾执行器**（§8.3-3）。
 *
 * 独立成文件（不塞进 rtdbd.c）：codegraph 评审指出 rtdbd.c 已是上帝对象
 * （120 节点 / calls 出度 326 / 入度 201），不应继续膨胀。
 * 本模块仍是**纯逻辑**：执行体通过函数指针表注入，不认识 fd/socket/VSOA。
 */
#ifndef RTDBD_DEFER_ACTIONS_H_
#define RTDBD_DEFER_ACTIONS_H_

#include <stdbool.h>
#include <stdint.h>

#include "session/rtdbd_session.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 每轮预算（§8.3-3「每轮预算」，照 conn_snapshot_pump 64 帧/轮先例）。 */
#define RTDBD_DEFER_DRAIN_BUDGET 32u

typedef struct {
    /* 目标会话当前 generation；目标不存在返回 0。 */
    uint32_t (*session_generation)(void *owner, uint32_t session_id);
    bool     (*session_alive)(void *owner, uint32_t session_id);
    /* 各类动作执行器；NULL = 该动作尚未实现 → 只计 unavailable，绝不假装执行。 */
    void (*on_reconnect)(void *owner, uint32_t session_id, uint32_t arg);
    void (*on_close)(void *owner, uint32_t session_id, uint32_t arg);
    void (*on_rebuild_subs)(void *owner, uint32_t session_id, uint32_t arg);
    void (*on_trigger_fullsync)(void *owner, uint32_t session_id, uint32_t arg);
    void (*on_peer_add)(void *owner, uint32_t session_id, uint32_t arg);
    void (*on_peer_del)(void *owner, uint32_t session_id, uint32_t arg);
    void *owner;
    /* 诊断计数（可为 NULL），对应 §11.1 P6 的 deferred_ops 等口径。 */
    uint64_t *executed;
    uint64_t *stale;        /* 目标已失效 → no-op */
    uint64_t *unavailable;  /* 动作执行器缺失 */
} rtdbd_defer_exec_t;

/* 判定动作是否已失效（会话级动作才校验 generation；集群级动作无会话寿命）。 */
bool rtdbd_defer_action_is_stale(const rtdbd_defer_action_t *a,
                                  const rtdbd_defer_exec_t *exec);

/* 循环尾统一执行：最多处理 budget 个动作（含 stale/unavailable），返回真正执行数。
 * 所有回调都由这里调用 —— **回调内不执行动作**，满足 SDK「不能在回调里 reconnect」。 */
uint32_t rtdbd_defer_actions_drain(rtdbd_defer_queue_t *q, uint32_t budget,
                                    const rtdbd_defer_exec_t *exec);

#ifdef __cplusplus
}
#endif
#endif /* RTDBD_DEFER_ACTIONS_H_ */
