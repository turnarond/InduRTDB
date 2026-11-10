/**
 * @file rtdbd_dispatch.c
 * @brief rtdbd transport 无关核心分发表（M1：双前端等价协议层）
 *
 * 仅含纯函数，无任何 transport / VSOA / 全局状态依赖，由
 * scripts/check_dispatch_boundary.py 机械校验零符号泄漏。doc 13 §7/§7.4。
 */
#include "rtdbd_dispatch.h"

#include "stats.h" /* g_stats（进程内共享统计，非传输） */

#include <string.h>

uint16_t rtdbd_st_to_vsoa_status(uint8_t st) {
    /* doc 13 §7.2：rtdbd status 用 0x80 + RTDBD_ST_*，7 个状态码无损映射。
     * 仅取低 7 位，避免未来状态码溢出到 VSOA 帧的命令/错误语义高位。 */
    return (uint16_t)(0x80u | (uint16_t)(st & 0x7Fu));
}

int rtdbd_opcode_from_url(const char *url, uint16_t *opcode) {
    /* doc 13 §7.1 镜像的 13 个能力（UDS-only 的管理通道不镜像）。
     * URL 规范、无歧义，线性匹配即可。 */
    static const struct {
        const char *url;
        uint16_t    op;
    } table[] = {
        {"/ping", 1},
        {"/point/write", 2},
        {"/point/batch_write", 20},
        {"/point/get", 10},
        {"/point/batch_read", 19},
        {"/subscription/add", 4},
        {"/subscription/remove", 5},
        {"/subscription/set", 18},
        {"/notify", 6},
        {"/snapshot_done", 22},
        {"/caps", 21},
        {"/select", 17},
        {"/health", 16},
    };
    if (!url || !opcode) {
        return -1;
    }
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); ++i) {
        if (strcmp(url, table[i].url) == 0) {
            *opcode = table[i].op;
            return 0;
        }
    }
    return -1;
}

const char *rtdbd_url_from_opcode(uint16_t opcode) {
    switch (opcode) {
        case 1:  return "/ping";
        case 2:  return "/point/write";
        case 20: return "/point/batch_write";
        case 10: return "/point/get";
        case 19: return "/point/batch_read";
        case 4:  return "/subscription/add";
        case 5:  return "/subscription/remove";
        case 18: return "/subscription/set";
        case 6:  return "/notify";
        case 22: return "/snapshot_done";
        case 21: return "/caps";
        case 17: return "/select";
        case 16: return "/health";
        default: return NULL;
    }
}

/* 写入点位（自 rtdbd.c 的 do_write 搬迁）：量程位计算 + 一次性落值。
 * 纯逻辑，不含传输依赖。返回 0 成功；-99 表示请求不支持（→ BAD_REQUEST）。 */
static int dispatch_do_write(indurtdb_t *h, const rtdbd_write_req_t *w,
                             indurtdb_meta_t *meta)
{
    memset(meta, 0, sizeof(*meta));

    /* 不支持的类型（STRING 受 8B 负载限制等）→ BAD_REQUEST */
    if (w->type == RTDBD_TYPE_STRING || w->type > RTDBD_TYPE_FLOAT) {
        return -99;
    }

    /* 取该点 meta（rtdbd 作为写权威，读 meta 属合理开销；核心热路径零 meta 读不变式保持） */
    uint8_t lim = INDURTDB_LIMIT_NONE;
    if (indurtdb_h_get_meta(h, w->point_id, meta) == 0) {
        double v = indurtdb_value_to_double(w->type, &w->value_bits);
        lim = indurtdb_eurange_limit(!!(meta->flags & INDURTDB_META_FLAG_EUR),
                                     meta->eur_min, meta->eur_max, v);
    }

    /* 一次性落值 + 量程位（quality 基础码恒为 GOOD） */
    uint8_t quality = INDURTDB_QUALITY_MAKE(INDURTDB_QUALITY_GOOD, lim);
    return indurtdb_h_write_quality_ts(h, w->point_id, w->type, &w->value_bits,
                                       w->source_ts_ns, quality);
}

uint16_t rtdbd_dispatch_write(indurtdb_t *h, const irt_policy_t *policy,
                               const rtdbd_conf_t *conf,
                               const rtdbd_write_req_t *w,
                               uint32_t uid, uint32_t partition,
                               indurtdb_meta_t *out_meta)
{
    indurtdb_meta_t local_meta;
    if (!out_meta) {
        out_meta = &local_meta;
    }
    memset(out_meta, 0, sizeof(*out_meta));

    if (!h || !policy || !conf || !w) {
        return RTDBD_ST_BAD_REQUEST;
    }

    /* 鉴权：deny by default，与 UDS 侧**同一函数**、同一顺序（§8.3-4）。 */
    if (!irt_policy_allows(policy, uid, partition, w->point_id)) {
        g_stats.write_rejected++;
        rtdbd_stats_note_error();
        return RTDBD_ST_DENIED;
    }
    if (!rtdbd_conf_partition_allowed(conf, partition)) {
        g_stats.write_rejected++;
        rtdbd_stats_note_error();
        return RTDBD_ST_DENIED;
    }

    int rc = dispatch_do_write(h, w, out_meta);
    if (rc == -99) {
        return RTDBD_ST_BAD_REQUEST;
    }
    if (rc != 0) {
        g_stats.write_error++;
        rtdbd_stats_note_error();
        return RTDBD_ST_INTERNAL;
    }

    g_stats.n_writes++;
    /* 审计与变更通知由前端负责（传输特有副作用），见 dispatch.h 的拆分表。 */
    return RTDBD_ST_OK;
}

uint16_t rtdbd_dispatch_subscribe_authz(indurtdb_t *h, const irt_policy_t *policy,
                                         const rtdbd_conf_t *conf,
                                         uint32_t uid, uint32_t point_id)
{
    if (!h || !policy || !conf) {
        return RTDBD_ST_BAD_REQUEST;
    }
    indurtdb_point_t pt;
    if (indurtdb_h_read_point(h, point_id, &pt) != 0) {
        return RTDBD_ST_NOT_FOUND; /* 越界 id */
    }
    if (pt.name[0] == '\0') {
        return RTDBD_ST_NOT_FOUND; /* 未注册点位不可订阅，与 LIST 一致 */
    }
    /* 订阅鉴权用**点位所属分区**（与写路径的连接分区不同） */
    if (!irt_policy_allows(policy, uid, pt.partition, point_id)) {
        return RTDBD_ST_DENIED;
    }
    if (!rtdbd_conf_partition_allowed(conf, pt.partition)) {
        return RTDBD_ST_DENIED;
    }
    return RTDBD_ST_OK;
}

uint16_t rtdbd_dispatch_notify(indurtdb_t *h, const rtdbd_write_req_t *w,
                                rtdbd_notify_t *out_notify)
{
    if (!h || !w || !out_notify) {
        return 1 /* RTDBD_ST_BAD_REQUEST */;
    }
    rtdbd_notify_t nt;
    memset(&nt, 0, sizeof(nt));
    nt.point_id     = w->point_id;
    nt.type         = w->type;
    nt.value_bits   = w->value_bits;
    nt.source_ts_ns = w->source_ts_ns;
    nt.timestamp_ns = 0;

    /* 时间戳与质量取当前点值：写路径可能把 TIMEOUT 恢复为 GOOD，通知必须携带
     * 当前质量（读点而非假定 GOOD，可保留量程位）。 */
    indurtdb_point_t pt;
    if (indurtdb_h_read_point(h, w->point_id, &pt) == 0) {
        nt.timestamp_ns = pt.timestamp_ns;
        nt.reserved[0]  = pt.quality;
    }
    memcpy(out_notify, &nt, sizeof(nt));
    return 0 /* RTDBD_ST_OK */;
}

bool rtdbd_dispatch_notify_rbe(rtdbd_subtab_t *sub, const rtdbd_write_req_t *w,
                               const indurtdb_meta_t *meta)
{
    if (!sub || !w) {
        return false;
    }
    /* 当前值（double）用于 RBE 死区比较 */
    double v = indurtdb_value_to_double(w->type, &w->value_bits);
    return rtdbd_subtab_decide(sub, w->point_id, v, meta);
}

uint16_t rtdbd_dispatch_get(indurtdb_t *h, uint32_t point_id,
                            uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (resp_len) {
        *resp_len = 0;
    }
    if (!h || !resp) {
        return 1 /* RTDBD_ST_BAD_REQUEST */;
    }
    indurtdb_point_t pt;
    if (indurtdb_h_read_point(h, point_id, &pt) != 0) {
        return 4 /* RTDBD_ST_NOT_FOUND */; /* 越界 id */
    }
    if (pt.name[0] == '\0') {
        return 4 /* RTDBD_ST_NOT_FOUND */; /* 未注册点位不可读，与 LIST 一致 */
    }
    if (resp_cap < sizeof(rtdbd_get_resp_t)) {
        return 5 /* RTDBD_ST_NO_MEM */; /* 缓冲不足：不写越界 */
    }
    rtdbd_get_resp_t r;
    memset(&r, 0, sizeof(r));
    r.point_id       = point_id;
    r.type           = pt.type;
    r.quality        = pt.quality;
    memcpy(&r.value_bits, &pt.value, sizeof(r.value_bits));
    if (pt.type == INDURTDB_TYPE_STRING) {
        memcpy(r.value_str, pt.value.str, sizeof(r.value_str));
    }
    r.timestamp_ns = pt.timestamp_ns;
    r.source_ts_ns = pt.source_timestamp_ns;
    memcpy(resp, &r, sizeof(r));
    if (resp_len) {
        *resp_len = sizeof(r);
    }
    return 0 /* RTDBD_ST_OK */;
}

uint16_t rtdbd_dispatch_caps(const rtdbd_dispatch_cfg_t *cfg,
                             uint8_t *resp, size_t resp_cap, size_t *resp_len)
{
    if (resp_len) {
        *resp_len = 0;
    }
    if (!cfg || !resp) {
        return 1 /* RTDBD_ST_BAD_REQUEST */;
    }
    if (resp_cap < sizeof(rtdbd_caps_t)) {
        return 5 /* RTDBD_ST_NO_MEM */; /* 缓冲不足：不写越界 */
    }
    rtdbd_caps_t caps;
    memset(&caps, 0, sizeof(caps));
    caps.proto_version         = RTDBD_PROTO_VERSION;
    caps.max_clients           = cfg->max_clients;
    caps.initial_clients       = cfg->initial_clients;
    caps.max_subs_per_conn     = cfg->max_subs_per_conn;
    caps.initial_subs_per_conn = cfg->initial_subs_per_conn;
    caps.max_batch             = cfg->max_batch;
    caps.flags                 = RTDBD_CAPS_F_BATCH_READ |
                                 RTDBD_CAPS_F_BATCH_WRITE |
                                 RTDBD_CAPS_F_SNAPSHOT_DONE;
    memcpy(resp, &caps, sizeof(caps));
    if (resp_len) {
        *resp_len = sizeof(caps);
    }
    return 0 /* RTDBD_ST_OK */;
}

bool rtdbd_normalize_url(const char *url, char *out, size_t out_cap)
{
    if (!url || !out || out_cap == 0) {
        return false;
    }
    out[0] = '\0';

    /* 版本段形态："/<段>/v<数字>[/<能力路径>]"（doc 13 §95「URL 带版本段」）。
     * 剥离首个 "/v<数字>" 段，得到裸能力路径；无版本段则原样复制。 */
    const char *p1 = strchr(url + 1, '/');
    if (p1 && p1[1] == 'v' && p1[2] >= '0' && p1[2] <= '9') {
        const char *p2 = strchr(p1 + 1, '/');
        const char *src = p2 ? p2 : url + strlen(url); /* 无能力路径 → 空串 */
        size_t n = strlen(src);
        if (n + 1 > out_cap) {
            return false;
        }
        memcpy(out, src, n + 1);
        return true;
    }

    size_t n = strlen(url);
    if (n + 1 > out_cap) {
        return false;
    }
    memcpy(out, url, n + 1);
    return true;
}

uint16_t rtdbd_dispatch(uint16_t opcode,
                        const uint8_t *req, size_t req_len,
                        uint8_t *resp, size_t resp_cap, size_t *resp_len) {
    (void)req;
    (void)req_len;
    (void)resp;
    (void)resp_cap;
    if (resp_len) {
        *resp_len = 0;
    }
    if (opcode == 1 /* RTDBD_OP_PING */) {
        /* 无请求负载期望；响应体为空 + ST_OK。transport 无关、确定性。 */
        return 0 /* RTDBD_ST_OK */;
    }
    /* 其余 opcode 暂未迁移：占位 NOT_IMPLEMENTED，后续里程碑把 rtdbd.c 的
     * op_* 逻辑迁入此处并接入 ctx；UDS 侧 22 opcode 与 ABI 零变化（doc 13 §8.3-1）。 */
    return 9 /* RTDBD_ST_NOT_IMPLEMENTED */;
}
