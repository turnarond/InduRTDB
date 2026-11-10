/**
 * @file rtdbd_defer_actions.c
 * @brief v3.12 M3.2 ③：延迟动作**循环尾统一执行**（§8.3-3）。
 *
 * 纯逻辑：执行体由 rtdbd_defer_exec_t 函数指针表注入，本文件不认识
 * fd / socket / VSOA / rtdbd_conn_t。所有动作都在这里（而非回调里）执行，
 * 满足 SDK「不能在回调里 reconnect」约束。
 */
#include "cluster/rtdbd_defer_actions.h"

#include <string.h>

/* 只有「会话级」动作受目标会话寿命约束；集群级动作（peer/全量）不校验
 * generation —— 它们不指向某个连接槽位，槽位复用与它们无关。 */
static bool targets_session(rtdbd_defer_kind_t kind)
{
    return kind == RTDBD_DEFER_RECONNECT ||
           kind == RTDBD_DEFER_CLOSE ||
           kind == RTDBD_DEFER_REBUILD_SUBS;
}

bool rtdbd_defer_action_is_stale(const rtdbd_defer_action_t *a,
                                  const rtdbd_defer_exec_t *exec)
{
    if (!a || !exec) {
        return true;
    }
    if (!targets_session(a->kind)) {
        return false;
    }
    if (a->target_generation == 0u) {
        return false; /* 旧入队点未启用校验 */
    }
    if (!exec->session_alive || !exec->session_alive(exec->owner, a->session_id)) {
        return true; /* 目标已不存在 */
    }
    if (!exec->session_generation) {
        return true; /* 无法判定 → 保守 no-op */
    }
    return exec->session_generation(exec->owner, a->session_id) != a->target_generation;
}

static void (*pick_handler(const rtdbd_defer_exec_t *e, rtdbd_defer_kind_t k))
    (void *, uint32_t, uint32_t)
{
    switch (k) {
        case RTDBD_DEFER_RECONNECT:        return e->on_reconnect;
        case RTDBD_DEFER_CLOSE:            return e->on_close;
        case RTDBD_DEFER_REBUILD_SUBS:     return e->on_rebuild_subs;
        case RTDBD_DEFER_TRIGGER_FULLSYNC: return e->on_trigger_fullsync;
        case RTDBD_DEFER_PEER_ADD:         return e->on_peer_add;
        case RTDBD_DEFER_PEER_DEL:         return e->on_peer_del;
        case RTDBD_DEFER_NONE:
        default:                           return NULL;
    }
}

uint32_t rtdbd_defer_actions_drain(rtdbd_defer_queue_t *q, uint32_t budget,
                                    const rtdbd_defer_exec_t *exec)
{
    if (!q || !exec || budget == 0u) {
        return 0u;
    }
    uint32_t done = 0u;
    for (uint32_t i = 0u; i < budget; ++i) {
        rtdbd_defer_action_t a;
        memset(&a, 0, sizeof(a));
        if (!rtdbd_defer_pop(q, &a)) {
            break;
        }
        if (rtdbd_defer_action_is_stale(&a, exec)) {
            if (exec->stale) {
                (*exec->stale)++;
            }
            continue;
        }
        void (*fn)(void *, uint32_t, uint32_t) = pick_handler(exec, a.kind);
        if (!fn) {
            /* 执行器缺失：入队不丢、drain 只计 unavailable，绝不假装执行。 */
            if (exec->unavailable) {
                (*exec->unavailable)++;
            }
            continue;
        }
        fn(exec->owner, a.session_id, a.arg);
        if (exec->executed) {
            (*exec->executed)++;
        }
        done++;
    }
    return done;
}
