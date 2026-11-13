/**
 * @file rtdbd_session.c
 * @brief 传输无关会话 + 延迟动作队列实现（M1.1，doc 13 §8.3-3）
 *
 * 纯逻辑模块：不触碰传输 / VSOA 符号，不依赖 rtdbd 全局状态。
 */
#include "rtdbd_session.h"

#include <string.h>

bool rtdbd_session_init(rtdbd_session_t *s, uint32_t initial_sub_cap)
{
    if (!s) {
        return false;
    }
    memset(s, 0, sizeof(*s));
    if (initial_sub_cap == 0) {
        initial_sub_cap = RTDBD_SUBTAB_MIN_CAP;
    }
    return rtdbd_subtab_init(&s->sub, initial_sub_cap);
}

void rtdbd_session_reset(rtdbd_session_t *s)
{
    if (!s) {
        return;
    }
    /* 逐字段复位（等价既有 conn_init 语义）：不清 subs[] 数组内容，只置 nsubs=0
     * —— 数组容量大（RTDBD_SUB_MAX 项），整块 memset 会带来无谓的每连接开销。
     * 订阅表清空（保留容量，避免槽位复用时反复分配）。 */
    s->partition = 0;
    s->nsubs = 0;
    s->caps_flags = 0;
    s->caps_negotiated = false;
    s->snap_active = false;
    s->snap_next = 0;
    s->snap_emitted = 0;
    s->service_id[0] = '\0';
    s->epoch = 0;
    s->role = RTDBD_ROLE_UNKNOWN;
    s->cred.uid = 0;
    s->cred.pid = 0;
    s->cred.cred_ok = false;
    s->sink.send = NULL;
    s->sink.close = NULL;
    s->sink.owner = NULL;
    rtdbd_subtab_clear(&s->sub);
}

void rtdbd_session_free(rtdbd_session_t *s)
{
    if (!s) {
        return;
    }
    rtdbd_subtab_free(&s->sub);
    memset(s, 0, sizeof(*s));
}

bool rtdbd_session_set_service_id(rtdbd_session_t *s, const char *id)
{
    if (!s || !id) {
        return false;
    }
    size_t n = strlen(id);
    if (n == 0 || n >= sizeof(s->service_id)) {
        return false; /* 超长不截断、不写入（防身份混淆） */
    }
    memcpy(s->service_id, id, n + 1);
    return true;
}

bool rtdbd_session_is_sync_peer(const rtdbd_session_t *s)
{
    return s && s->role == RTDBD_ROLE_SYNC_PEER;
}

/* ---- 订阅集合（有序视图 + RBE 合并表）---- */

rtdbd_subs_add_result_t rtdbd_session_subs_add(rtdbd_session_t *s, uint32_t point_id,
                                                uint32_t max_subs)
{
    if (!s) {
        return RTDBD_SUBS_ADD_FAIL;
    }
    if (rtdbd_subtab_has(&s->sub, point_id)) {
        return RTDBD_SUBS_ADD_EXISTS; /* 幂等：已订阅不是错误 */
    }
    if (s->nsubs >= max_subs || s->nsubs >= RTDBD_SUB_MAX) {
        return RTDBD_SUBS_ADD_FAIL;
    }
    /* 合并表插入失败（表满）时不写有序视图，保持两份视图一致。 */
    if (!rtdbd_subtab_insert(&s->sub, point_id)) {
        return RTDBD_SUBS_ADD_FAIL;
    }
    s->subs[s->nsubs++] = point_id;
    return RTDBD_SUBS_ADD_OK;
}

void rtdbd_session_subs_remove(rtdbd_session_t *s, uint32_t point_id)
{
    if (!s) {
        return;
    }
    /* 退订同步清 RBE 状态（重新订阅首值必发）。 */
    rtdbd_subtab_remove(&s->sub, point_id);
    for (uint32_t i = 0; i < s->nsubs; ++i) {
        if (s->subs[i] != point_id) {
            continue;
        }
        s->subs[i] = s->subs[s->nsubs - 1]; /* 末位交换法（与既有 UDS 行为一致） */
        s->nsubs--;
        return;
    }
}

bool rtdbd_session_install_subs(rtdbd_session_t *s, const uint32_t *ids, uint32_t n)
{
    if (!s) {
        return false;
    }
    rtdbd_subtab_t next;
    if (!rtdbd_subtab_init(&next, RTDBD_SUBTAB_MIN_CAP)) {
        return false;
    }
    if (n > 0 && !rtdbd_subtab_reserve(&next, n * 2u)) {
        rtdbd_subtab_free(&next);
        return false;
    }
    for (uint32_t i = 0; i < n; ++i) {
        if (!rtdbd_subtab_insert(&next, ids[i])) {
            rtdbd_subtab_free(&next);
            return false; /* 旧订阅集保持不变 */
        }
    }
    rtdbd_subtab_free(&s->sub);
    s->sub = next;
    if (n > 0 && ids) {
        memcpy(s->subs, ids, (size_t)n * sizeof(uint32_t));
    }
    s->nsubs = n;
    return true;
}

uint32_t rtdbd_session_subs_count(const rtdbd_session_t *s)
{
    return s ? s->nsubs : 0u;
}

bool rtdbd_session_subs_get(const rtdbd_session_t *s, uint32_t idx, uint32_t *out_point_id)
{
    if (!s || !out_point_id || idx >= s->nsubs) {
        return false;
    }
    *out_point_id = s->subs[idx];
    return true;
}

bool rtdbd_session_subs_contains(const rtdbd_session_t *s, uint32_t point_id)
{
    return s && rtdbd_subtab_has(&s->sub, point_id);
}

bool rtdbd_session_subs_consistent(const rtdbd_session_t *s)
{
    if (!s) {
        return false;
    }
    if (s->sub.count != s->nsubs) {
        return false;
    }
    for (uint32_t i = 0; i < s->nsubs; ++i) {
        if (!rtdbd_subtab_has(&s->sub, s->subs[i])) {
            return false;
        }
    }
    return true;
}

void rtdbd_defer_queue_init(rtdbd_defer_queue_t *q)
{
    if (!q) {
        return;
    }
    memset(q, 0, sizeof(*q));
}

bool rtdbd_defer_push_gen(rtdbd_defer_queue_t *q, rtdbd_defer_kind_t kind,
                          uint32_t session_id, uint32_t arg,
                          uint32_t target_generation)
{
    if (!q) {
        return false;
    }
    if (kind == RTDBD_DEFER_NONE || kind > RTDBD_DEFER_PEER_DEL) {
        return false; /* 非法动作不入队 */
    }
    if (q->count >= RTDBD_DEFER_QUEUE_CAP) {
        q->dropped++; /* 队列满：拒绝并计数，不静默丢弃 */
        return false;
    }
    rtdbd_defer_action_t *a = &q->slots[q->tail];
    a->kind              = kind;
    a->session_id        = session_id;
    a->arg               = arg;
    a->seq               = q->enqueued;
    a->target_generation = target_generation;
    q->tail = (q->tail + 1u) % RTDBD_DEFER_QUEUE_CAP;
    q->count++;
    q->enqueued++;
    return true;
}

bool rtdbd_defer_push(rtdbd_defer_queue_t *q, rtdbd_defer_kind_t kind,
                      uint32_t session_id, uint32_t arg)
{
    /* 旧 API 保留兼容：等价于「不启用 generation 校验」（0）。 */
    return rtdbd_defer_push_gen(q, kind, session_id, arg, 0u);
}

bool rtdbd_defer_pop(rtdbd_defer_queue_t *q, rtdbd_defer_action_t *out)
{
    if (!q || !out) {
        return false;
    }
    if (q->count == 0) {
        return false;
    }
    *out = q->slots[q->head];
    q->head = (q->head + 1u) % RTDBD_DEFER_QUEUE_CAP;
    q->count--;
    q->executed++;
    return true;
}

uint32_t rtdbd_defer_count(const rtdbd_defer_queue_t *q)
{
    return q ? q->count : 0u;
}

const char *rtdbd_defer_kind_name(rtdbd_defer_kind_t kind)
{
    switch (kind) {
        case RTDBD_DEFER_RECONNECT:        return "reconnect";
        case RTDBD_DEFER_CLOSE:            return "close";
        case RTDBD_DEFER_REBUILD_SUBS:     return "rebuild_subs";
        case RTDBD_DEFER_TRIGGER_FULLSYNC: return "trigger_fullsync";
        case RTDBD_DEFER_PEER_ADD:         return "peer_add";
        case RTDBD_DEFER_PEER_DEL:         return "peer_del";
        case RTDBD_DEFER_NONE:
        default:                           return "none";
    }
}

/* ---- v3.12 M3.3 切片4：NOTIFY 门控诊断计数（纯逻辑，普通 ++）----
 * 与 defer_queue 的 dropped/enqueued/executed 同范式：doc 13 §8.3-3 单线程
 * 统一 poll、零线程零锁，故不引入原子操作。
 * ctr 只是诊断出口：NULL 时仍须正常完成门控判定并允许发送，仅在 ++ 处判空。 */

rtdbd_notify_gate_t rtdbd_session_notify_gate(rtdbd_session_t *s, uint32_t point_id,
        rtdbd_notify_rbe_fn rbe, void *rbe_ctx, rtdbd_notify_counters_t *ctr)
{
    if (!s) {
        return RTDBD_NOTIFY_GATE_SKIP; /* 空会话：安全拒绝（ctr 为诊断出口，不改控制流） */
    }
    /* 顺序不可变：先过滤，再调 RBE。rtdbd_dispatch_notify_rbe 有副作用
     * （推进 subtab 状态），绝不能提前，否则未订阅 / sync_peer 会消耗 RBE 状态。 */
    if (rtdbd_session_is_sync_peer(s)) {
        return RTDBD_NOTIFY_GATE_SKIP; /* 防回环：sync_peer 不得收 NOTIFY */
    }
    if (!rtdbd_session_subs_contains(s, point_id)) {
        return RTDBD_NOTIFY_GATE_SKIP; /* 未订阅该点 */
    }
    /* 缺少 RBE 判定器属调用方契约违约：归入 SKIP 且不触碰任何计数，
     * 不得伪装成 RBE 死区抑制而计 rbe_gate_misses。 */
    if (!rbe) {
        return RTDBD_NOTIFY_GATE_SKIP;
    }
    const bool pass = rbe(rbe_ctx, s, point_id); /* 仅合法会话算 RBE */
    if (!pass) {
        if (ctr) {
            ctr->rbe_gate_misses++;
        }
        return RTDBD_NOTIFY_GATE_MISS; /* RBE 死区抑制 */
    }
    if (ctr) {
        ctr->rbe_gate_hits++;
    }
    return RTDBD_NOTIFY_GATE_HIT;
}

/* 发送点护栏：正常路径不可达（gate 已过滤非法会话），仅作防御 +
 * 使 peer_notify_leak 计数出口可观测。保留合理，不删除（评审 M-4）。 */
bool rtdbd_session_notify_deliver_guard(const rtdbd_session_t *s, uint32_t point_id,
        rtdbd_notify_counters_t *ctr)
{
    if (!s) {
        return false; /* 空会话：安全拒绝（ctr 为诊断出口，不改控制流） */
    }
    if (rtdbd_session_is_sync_peer(s) ||
        !rtdbd_session_subs_contains(s, point_id)) {
        if (ctr) {
            ctr->peer_notify_leak++; /* 非订阅者/sync_peer 差点收包 → 记泄露，绝不外发 */
        }
        return false;
    }
    return true;
}

/* VSOA 生产路径未接线，当前恒 0；当前无生产调用点；属切片 5/IR1，见变更卡留白。
 * 本函数是供验收取证的纯逻辑 API，不由任何生产路径调用。 */
void rtdbd_notify_counters_record_snapshot_done_caps_only(rtdbd_notify_counters_t *ctr)
{
    if (ctr) {
        ctr->snapshot_done_caps_only++;
    }
}

void rtdbd_notify_counters_snapshot(const rtdbd_notify_counters_t *ctr,
                                    rtdbd_notify_counters_t *out)
{
    if (!out) {
        return;
    }
    if (!ctr) {
        memset(out, 0, sizeof(*out));
        return;
    }
    *out = *ctr; /* 纯标量逐字段值拷贝，非别名 */
}

void rtdbd_notify_counters_reset(rtdbd_notify_counters_t *ctr)
{
    if (ctr) {
        memset(ctr, 0, sizeof(*ctr));
    }
}
