/**
 * @file rtdbd_peers_apply.c
 * @brief v3.12 M3.3 切片 C：成员表整组替换 + ADD/DEL diff 入队（UDS/VSOA 共用）。
 *
 * 单独成文件，使 rtdbd_peers_global.c 保持纯轻量单例（纯逻辑单测零额外依赖）。
 */
#include "rtdbd_peers_global.h"
#include "rtdbd_defer_global.h"

/* 旧表快照（静态，主循环单线程；两前端串行调用，无并发）。 */
static rtdbd_peer_table_t g_peers_prev;

int rtdbd_peers_apply_set(const rtdbd_peer_t *peers, uint32_t n)
{
    if (n > RTDBD_PEER_MAX) {
        return -1;
    }
    rtdbd_peer_table_t *tbl = rtdbd_peers_global();
    const char *self = rtdbd_peers_self_addr();
    rtdbd_defer_queue_t *dq = rtdbd_defer_global();

    /* 最小 lag 计算（§8.3-6 lag 精确语义未裁决，仅落地本节点视角滞后量：
     * lag = local_offset − peer.applied_offset，饱和 0）。boot_id 透传（来自上报节点）。 */
    uint64_t local = rtdbd_peers_local_offset();
    rtdbd_peer_t buf[RTDBD_PEER_MAX];
    for (uint32_t i = 0; i < n; ++i) {
        buf[i] = peers[i];
        buf[i].lag = rtdbd_peer_lag(local, buf[i].applied_offset); /* 最小 lag（§8.3-6 语义未裁决） */
    }

    g_peers_prev = *tbl;
    if (rtdbd_peer_table_set(tbl, buf, n, self[0] ? self : NULL) != 0) {
        return -1; /* 旧表保持不变（§8.3-6） */
    }

    /* ADD/DEL 一律入队，由主循环循环尾统一执行；generation=0：集群级动作
     * 不指向连接槽位，不受槽位复用影响。 */
    bool any_new = false;
    for (uint32_t i = 0; i < tbl->count; ++i) {
        if (!rtdbd_peer_table_get(&g_peers_prev, tbl->peers[i].addr)) {
            (void)rtdbd_defer_push_gen(dq, RTDBD_DEFER_PEER_ADD, 0u, i, 0u);
            any_new = true;
        }
    }
    if (any_new) {
        (void)rtdbd_defer_push_gen(dq, RTDBD_DEFER_TRIGGER_FULLSYNC, 0u, 0u, 0u);
    }
    for (uint32_t i = 0; i < g_peers_prev.count; ++i) {
        if (!rtdbd_peer_table_get(tbl, g_peers_prev.peers[i].addr)) {
            (void)rtdbd_defer_push_gen(dq, RTDBD_DEFER_PEER_DEL, 0u, i, 0u);
        }
    }
    return 0;
}
