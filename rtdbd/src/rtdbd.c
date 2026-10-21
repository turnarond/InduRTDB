/**
 * @file rtdbd.c
 * @brief rtdbd —— 写权威服务进程（控制面 OT）
 *
 * T1 骨架：UDS 监听 + 协议解析 + 串行处理。
 * 当前为 TDD 的「红」阶段骨架：WRITE 返回 NOT_IMPLEMENTED，
 * 待 T1 实现提交后替换为真实写入路径（串行写 + 鉴权 + 审计）。
 */
#include <errno.h>
#include <getopt.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <indurtdb/indurtdb.h>
#include <rtdbd/protocol.h>

#include "audit.h"
#include "policy.h"
#include "rtdbd_conf.h"
#include "logbuf.h"
#include "subtab.h"
#include "stats.h"

/* 线结构 rtdbd_meta_payload_t 与库结构 indurtdb_meta_t 必须逐字节同尺寸，
 * 否则 rtdbd.c 的 memcpy(&m, &s.meta, sizeof(m)) 会越界/截断（protocol.h:108）。 */
RTDBD_STATIC_ASSERT(sizeof(rtdbd_meta_payload_t) == sizeof(indurtdb_meta_t),
               "rtdbd_meta_payload_t must match indurtdb_meta_t size");

#define RTDBD_MAX_CLIENTS 128
#define RTDBD_SHUTDOWN_DELAY_SEC 1

/* --max-points 的硬上限（B3 参数校验）：点位区按 128B/点计算，
 * 100 万点 ≈ 128MB；再大既无实用意义，也会让 layout 的 uint32 偏移与
 * mmap/ftruncate 规模失控。用于挡住 -1 → UINT32_MAX 之类的非法输入。 */
#define RTDBD_MAX_POINTS_LIMIT 1000000u

/* 语义退出码（v3.7 主题B B2/B3）：区分致命错误，供 supervisor/systemd 判定。
 * 2（配置）与 4（自检）为致命：supervisor 停止 respawn，避免坏配置无限重启。 */
#define RTDBD_EXIT_OK        0
#define RTDBD_EXIT_ERROR     1  /* 通用 / 启动错误（参数解析、policy 加载失败等） */
#define RTDBD_EXIT_CONFIG    2  /* 配置校验失败（B3 fail-fast） */
#define RTDBD_EXIT_SHM       3  /* 共享内存损坏 / 初始化失败 */
#define RTDBD_EXIT_SELFCHECK 4  /* 启动自检失败 */

static volatile sig_atomic_t g_stop = 0;
/* v3.7 主题B B1：SIGUSR1 请求 dump 运行统计。handler 只置标志，
 * 实际 dump 在主循环内执行（信号安全：handler 内不得做 IO/格式化）。 */
static volatile sig_atomic_t g_dump_stats = 0;
/* dump 输出路径（--stats-file 可配，默认 /tmp/rtdbd.stats） */
static char g_stats_file[256] = "/tmp/rtdbd.stats";

/* 监控 LIST 用的实例点位上限与分片缓冲（rtdbd 串行处理，静态缓冲安全） */
static uint32_t g_max_points = 0;
#define RTDBD_LIST_CHUNK 1024u
static uint8_t g_list_buf[RTDBD_LIST_CHUNK * sizeof(rtdbd_point_info_t)];

/* v3.9 T5：进程唯一实例句柄（句柄 API）。所有库调用经此句柄；
 * 分区感知 API（_in 族）只存在于句柄层，故 rtdbd 必须持句柄。
 * 启动后立刻赋值，服务期只读。 */
static indurtdb_t* g_h = NULL;

/* 运行日志环形缓冲 + GET_LOG 导出缓冲（定长，无堆分配） */
static irt_logbuf_t      g_logbuf;
static rtdbd_log_entry_t g_log_out[RTDBD_LOG_CAPACITY];

/* SET_SUBS 读取 id 数组的静态缓冲（BSS，单线程串行处理，无并发） */
static uint32_t g_set_subs_ids[RTDBD_SUB_MAX];

static void on_signal(int sig)
{
    /* 信号处理器只置 volatile sig_atomic_t 标志（信号安全：不做 IO、不格式化） */
    if (sig == SIGUSR1) { g_dump_stats = 1; return; }
    g_stop = 1;
}

static int send_all(int fd, const void* buf, size_t len)
{
    const char* p = (const char*)buf;
    while (len > 0) {
        ssize_t n = send(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static int recv_all(int fd, void* buf, size_t len)
{
    char* p = (char*)buf;
    while (len > 0) {
        ssize_t n = recv(fd, p, len, 0);
        if (n < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        if (n == 0) return -1; /* 对端关闭 */
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

/* ---- 连接状态：每连接的订阅表 + 出站队列（定长，无堆分配） ---- */

/* 每连接出站通知队列容量（v3.7 主题B B2 背压）。
 * 定长环形队列：队列满时丢最旧（保序），绝不无限增长拖垮服务端。 */
#define RTDBD_OUTQ_CAP 1024u

typedef struct {
    int      fd;
    uint32_t partition;   /* v3.9 T5：连接级默认分区（OP_SELECT 设置，初值 0） */
    uint32_t subs[RTDBD_SUB_MAX];
    uint32_t nsubs;
    rtdbd_subtab_t sub;   /* v3.10：订阅成员 + RBE 状态合并表 */
    /* 出站通知环形队列（B2 背压）：写成功只入队，由主循环 POLLOUT 时 flush。
     * 慢消费者 socket 缓冲满 → POLLOUT 不就绪 → 不 flush（不阻塞他人），
     * 队列填满则丢最旧并计 notify_drop。
     * outq_sent：队首帧已发出的字节数（部分发送进度），非阻塞 flush 用。 */
    rtdbd_notify_t outq[RTDBD_OUTQ_CAP];
    uint16_t       outq_head;
    uint16_t       outq_tail;
    uint16_t       outq_count;
    uint16_t       outq_sent;
} rtdbd_conn_t;

/* 一帧通知的线缆长度：请求头 12B + 负载 32B */
#define RTDBD_NOTIFY_FRAME_LEN (sizeof(rtdbd_req_hdr_t) + sizeof(rtdbd_notify_t))

/* 全局连接表：放文件域（.bss）而非 main 栈帧。
 * v3.10：每连接 subs[1024] + subtab(2048×16B) + outq(1024×32B) ≈ 68KB，
 * 128 连接约 8.5MB；留在栈上对嵌入式（SylixOS，小栈配置）不可行。
 * 零堆、零栈压力。 */
static rtdbd_conn_t g_conns[RTDBD_MAX_CLIENTS];

static void conn_init(rtdbd_conn_t* c)
{
    c->fd = -1;
    c->partition = 0;   /* v3.9 T5：连接默认分区复位（断开/复用槽位时清） */
    c->nsubs = 0;
    rtdbd_subtab_init(&c->sub);
    c->outq_head  = 0;
    c->outq_tail  = 0;
    c->outq_count = 0;
    c->outq_sent  = 0;
}

static bool conn_has_sub(const rtdbd_conn_t* c, uint32_t point_id)
{
    return rtdbd_subtab_has(&c->sub, point_id);
}

static int conn_add_sub(rtdbd_conn_t* c, uint32_t point_id, uint32_t max_subs)
{
    if (conn_has_sub(c, point_id)) return 0;
    if (c->nsubs >= max_subs || c->nsubs >= RTDBD_SUB_MAX) return -1;
    /* subtab 插入失败（表满）时不写 subs[]，保持两套视图一致。 */
    if (!rtdbd_subtab_insert(&c->sub, point_id)) return -1;
    c->subs[c->nsubs++] = point_id;
    return 0;
}

static void conn_del_sub(rtdbd_conn_t* c, uint32_t point_id)
{
    /* 退订同步清 RBE 状态（重新订阅首值必发）。 */
    rtdbd_subtab_remove(&c->sub, point_id);
    for (uint32_t i = 0; i < c->nsubs; ++i) {
        if (c->subs[i] != point_id) continue;
        c->subs[i] = c->subs[c->nsubs - 1];
        c->nsubs--;
        return;
    }
}

/* 丢弃某连接的整个出站队列（连接关闭 / flush 失败）。
 * 丢弃量必须并入 notify_drop，否则运维看到 n_writes 在涨、却既没收到通知、
 * notify_drop 又是 0，排查会被直接带偏（不可观测的静默丢失）。 */
static void conn_discard_outq(rtdbd_conn_t* c)
{
    if (c->outq_count > 0) {
        g_stats.notify_drop += c->outq_count;
        /* 丢弃未送达的通知是错误类事件：不计入窗口的话，运维会看到
         * n_writes 在涨、通知收不到、而 health 仍是 OK，排查被带偏。 */
        rtdbd_stats_note_error();
    }
    c->outq_count = 0;
    c->outq_head  = 0;
    c->outq_tail  = 0;
    c->outq_sent  = 0;
}

/* SET_SUBS 替换订阅集时的软清理：仅复位出站队列指针，**不**计入 notify_drop、
 * **不**调用 rtdbd_stats_note_error()。用于整组替换时丢弃旧快照/待发通知，
 * 这属正常业务流程而非背压丢弃。 */
static void conn_clear_outq_soft(rtdbd_conn_t* c)
{
    c->outq_count = 0;
    c->outq_head  = 0;
    c->outq_tail  = 0;
    c->outq_sent  = 0;
}

/* 出站队列入队（B2 背压）：满则丢最旧（保序），绝不无限增长。 */
static void outq_push(rtdbd_conn_t* c, const rtdbd_notify_t* nt)
{
    if (c->outq_count == RTDBD_OUTQ_CAP) {
        /* 队首已部分上线（socket 缓冲不足导致 outq_sent > 0）时**禁止淘汰**它：
         * 否则下轮 flush 会用新队首的帧、却从旧的 outq_sent 偏移续发，
         * 线上变成「旧帧前缀 + 新帧后缀」的混合帧（长度仍是 44B，
         * 但 magic/opcode/payload 已被拼接破坏，是对端无法解析的坏帧）。
         * 此时改为丢弃**本次新通知**——它最旧、价值最低，且尚未上线。 */
        if (c->outq_sent > 0) {
            g_stats.notify_drop++;
            rtdbd_stats_note_error();
            return;
        }
        c->outq_head = (uint16_t)((c->outq_head + 1u) % RTDBD_OUTQ_CAP);
        c->outq_count--;
        g_stats.notify_drop++;   /* 慢消费者：丢最旧，值仍在库内可被 GET 读到 */
        rtdbd_stats_note_error();
    }
    c->outq[c->outq_tail] = *nt;
    c->outq_tail = (uint16_t)((c->outq_tail + 1u) % RTDBD_OUTQ_CAP);
    c->outq_count++;
    g_stats.n_notifies++;   /* 入队计数（含触发丢弃的那次入队） */
}

/* 排空单个连接的出站队列，**非阻塞**。
 *
 * 用 MSG_DONTWAIT 逐段发送并记录 outq_sent 进度：
 * POLLOUT 就绪只保证发送缓冲有*部分*空间，不保证容纳得下整帧
 * （帧长 44B × CAP 64 = 2816B）。嵌入式（SylixOS）socket 缓冲常只有数 KB，
 * 若沿用阻塞 send_all，单个慢消费者会卡住整个单线程服务端——恰好在 B2
 * 要解决的场景下失效。故此处必须做到「能发多少发多少，剩下下轮再续」。
 *
 * 返回 0 = 已排空或本轮发不动（保留队列待下轮 POLLOUT）；-1 = 连接失效。
 */
static int outq_flush(rtdbd_conn_t* c)
{
    while (c->outq_count > 0) {
        /* 组帧：请求头 + 队首负载（仅在未发送完时构造，省常态开销） */
        uint8_t frame[RTDBD_NOTIFY_FRAME_LEN];
        rtdbd_req_hdr_t nh;
        memset(&nh, 0, sizeof(nh));
        nh.magic       = RTDBD_MAGIC;
        nh.version     = RTDBD_PROTO_VERSION;
        nh.opcode      = RTDBD_OP_NOTIFY;
        nh.payload_len = (uint32_t)sizeof(rtdbd_notify_t);
        memcpy(frame, &nh, sizeof(nh));
        memcpy(frame + sizeof(nh), &c->outq[c->outq_head], sizeof(rtdbd_notify_t));

        size_t total = RTDBD_NOTIFY_FRAME_LEN;
        size_t want  = total - c->outq_sent;
        ssize_t n = send(c->fd, frame + c->outq_sent, want, MSG_DONTWAIT);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;  /* 下轮再试 */
            conn_discard_outq(c);
            g_stats.notify_send_fail++;
            rtdbd_stats_note_error();
            return -1;
        }
        if (n == 0) return 0;   /* 理论上不会：非阻塞 send 返回 0 视为无进展 */
        c->outq_sent = (uint16_t)(c->outq_sent + (uint16_t)n);
        if (c->outq_sent < total) return 0;   /* 部分发送：下轮续 */

        /* 整帧发完 → 出队 */
        c->outq_head = (uint16_t)((c->outq_head + 1u) % RTDBD_OUTQ_CAP);
        c->outq_count--;
        c->outq_sent = 0;
    }
    return 0;
}

/* 写成功后向所有订阅了该点位的连接**入队**变更通知。
 * v3.7 主题A T1：经 RBE 门控——死区未越过则抑制本次入队（值仍已落库）。
 * v3.7 主题B B2：不再阻塞直发，改为入队 + 主循环 POLLOUT flush（背压）。 */
static void notify_broadcast(indurtdb_t* h, rtdbd_conn_t* conns, int n,
                             const rtdbd_write_req_t* w,
                             const indurtdb_meta_t* meta)
{
    rtdbd_notify_t nt;
    memset(&nt, 0, sizeof(nt));

    nt.point_id     = w->point_id;
    nt.type         = w->type;
    nt.value_bits   = w->value_bits;
    nt.source_ts_ns = w->source_ts_ns;
    nt.timestamp_ns = 0;

    /* 时间戳对所有订阅者相同，只读一次 */
    indurtdb_point_t pt;
    if (indurtdb_h_read_point(h, w->point_id, &pt) == 0) {
        nt.timestamp_ns = pt.timestamp_ns;
    }

    /* 当前值（double）用于 RBE 死区比较 */
    double v = indurtdb_value_to_double(w->type, &w->value_bits);

    for (int i = 0; i < n; ++i) {
        rtdbd_conn_t* c = &conns[i];
        if (c->fd < 0) continue;
        if (!conn_has_sub(c, w->point_id)) continue;

        /* RBE 门控：死区未越过则抑制本次通知 */
        if (!rtdbd_subtab_decide(&c->sub, w->point_id, v, meta)) continue;

        outq_push(c, &nt);
    }
}

/* v3.7 主题B B1：连接表完整性检查。
 * 空闲槽（fd<0）不应残留任何订阅（否则退订/断开清理有漏，幽灵订阅会持续收通知）；
 * 已连接槽的订阅数不得超过 RTDBD_SUB_MAX。返回 0 = 完好，-1 = 异常。 */
static int conn_table_check(const rtdbd_conn_t* conns, int n)
{
    for (int i = 0; i < n; ++i) {
        if (conns[i].nsubs > RTDBD_SUB_MAX) return -1;
        if (conns[i].fd < 0 && conns[i].nsubs != 0) return -1;  /* 空闲槽有残留订阅 */
    }
    return 0;
}

/* v3.7 主题B B1：自检 = 核心段头 + 计数分类（stats.c）。
 * 连接表完整性需连接表指针，由调用点（HEALTH opcode / SIGUSR1 dump，均在
 * conns 作用域内）叠加 conn_table_check()，避免把连接表提升为全局。
 * 结果写入 g_stats.health 并返回 RTDBD_HEALTH_*。 */
int rtdbd_self_check(void)
{
    return rtdbd_self_check_core(g_h);
}

/* v3.9 T5：rtdbd 私有配置校验。
 * 库内 `indurtdb_validate_config` 只有 v1 单例版（内部固定 g_default），无句柄版；
 * 而 T5 已迁移到句柄 API 且**不得改公共 API**，故在 rtdbd 侧用句柄接口复刻其行为：
 * 逐点读回，校验 type/access 合法 + 元数据语义。语义与库内实现等价。
 * 用法不变：返回 INDURTDB_CFG_OK 表示全合法；否则返回错误码并写 *bad_id。 */
static int rtdbd_validate_config(indurtdb_t* h, uint32_t* bad_id,
                                 uint32_t* err_field, const char** err_reason)
{
    if (bad_id)     *bad_id     = 0;
    if (err_field)  *err_field  = INDURTDB_CFG_FLD_NONE;
    if (err_reason) *err_reason = indurtdb_cfg_error_reason(INDURTDB_CFG_OK);

    if (!h || !indurtdb_h_is_initialized(h)) {
        if (err_reason) *err_reason = indurtdb_cfg_error_reason(INDURTDB_CFG_ERR_NULL);
        return INDURTDB_CFG_ERR_NULL;
    }

    for (uint32_t id = 0; id < g_max_points; ++id) {
        indurtdb_point_t pt;
        if (indurtdb_h_read_point(h, id, &pt) != 0) continue;  /* 未注册 */
        if (pt.name[0] == '\0') continue;                      /* 空名视为未注册 */

        int rc = INDURTDB_CFG_OK;
        if (pt.type > INDURTDB_TYPE_FLOAT) {
            rc = INDURTDB_CFG_ERR_TYPE;
            if (err_field) *err_field = INDURTDB_CFG_FLD_TYPE;
        } else if (pt.access != INDURTDB_ACCESS_READ_ONLY &&
                   pt.access != INDURTDB_ACCESS_READ_WRITE) {
            rc = INDURTDB_CFG_ERR_ACCESS;
            if (err_field) *err_field = INDURTDB_CFG_FLD_ACCESS;
        } else {
            indurtdb_meta_t m;
            memset(&m, 0, sizeof(m));
            (void)indurtdb_h_get_meta(h, id, &m);
            rc = indurtdb_validate_point_meta(&m, err_field);
        }

        if (rc != INDURTDB_CFG_OK) {
            if (bad_id)     *bad_id     = id;
            if (err_reason) *err_reason = indurtdb_cfg_error_reason(rc);
            return rc;
        }
    }
    return INDURTDB_CFG_OK;
}

/* 取对端进程凭据（pid / uid），用于鉴权与审计 */
static int peer_cred(int fd, uint32_t* pid, uint32_t* uid){
    struct ucred cr;
    socklen_t    len = sizeof(cr);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return -1;
    *pid = (uint32_t)cr.pid;
    *uid = (uint32_t)cr.uid;
    return 0;
}

/* 执行一次点位写入（携带采集时刻）。返回 0 成功，非 0 失败。
 * 同步计算 EURange 量程位并经质量感知入口落值；meta 在返回时填充（供通知侧 RBE 门控复用）。
 *
 * 采集时刻由客户端经协议传入；为 0 表示"未提供"，库侧退化为仅记录入库时刻。 */
static int do_write(indurtdb_t* h, const rtdbd_write_req_t* w, indurtdb_meta_t* meta)
{
    memset(meta, 0, sizeof(*meta));

    /* 不支持的类型（STRING 受 8B 负载限制等）→ BAD_REQUEST */
    if (w->type == RTDBD_TYPE_STRING || w->type > RTDBD_TYPE_FLOAT) return -99;

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

/* 返回 0 表示连接可继续保持；非 0 表示需关闭连接 */
/* ---- v3.5 监控只读通道：GET / LIST（读免鉴权） ---- */

/* 读取单点当前值快照；成功返回 0，失败（id 无效/未注册）返回 -1 */
static int do_get(indurtdb_t* h, uint32_t id, rtdbd_get_resp_t* out)
{
    indurtdb_point_t pt;
    if (indurtdb_h_read_point(h, id, &pt) != 0) return -1;
    if (pt.name[0] == '\0') return -1; /* 未注册点位不可读，与 LIST 一致 */

    memset(out, 0, sizeof(*out));
    out->point_id     = id;
    out->type         = pt.type;
    out->quality      = pt.quality;
    memcpy(&out->value_bits, &pt.value, sizeof(out->value_bits));
    if (pt.type == INDURTDB_TYPE_STRING)
        memcpy(out->value_str, pt.value.str, sizeof(out->value_str));
    out->timestamp_ns   = pt.timestamp_ns;
    out->source_ts_ns   = pt.source_timestamp_ns;
    return 0;
}

/* 枚举已注册点位（name[0]!='\0' 视为已注册）。
 * 从 offset(id) 起向后扫描，最多收集 max 个已注册点（max=0 表示上限 CHUNK 个），
 * 结果写入 buf，*out_len 返回字节数。分页时 offset 为起始 id，max 为返回条数上限。
 *
 * v3.9 T5：按分区过滤 —— filter == RTDBD_PARTITION_ALL 列出全部分区；
 * 否则仅列出 pt.partition == filter 的点位，且每条信息携带其 partition。 */
static void do_list(indurtdb_t* h, uint32_t filter, uint32_t max_n, uint32_t offset,
                    uint8_t* buf, size_t* out_len)
{
    *out_len = 0;
    if (offset >= g_max_points) return;

    uint32_t cap = (max_n == 0) ? RTDBD_LIST_CHUNK : max_n;
    if (cap > RTDBD_LIST_CHUNK) cap = RTDBD_LIST_CHUNK;

    rtdbd_point_info_t* arr = (rtdbd_point_info_t*)buf;
    uint32_t n = 0;
    for (uint32_t id = offset; id < g_max_points && n < cap; ++id) {
        indurtdb_point_t pt;
        if (indurtdb_h_read_point(h, id, &pt) != 0) continue;
        if (pt.name[0] == '\0') continue; /* 未注册点位跳过 */
        if (filter != RTDBD_PARTITION_ALL && pt.partition != filter) continue;

        rtdbd_point_info_t* e = &arr[n++];
        e->point_id = id;
        e->type     = pt.type;
        e->access   = pt.access;
        e->partition = pt.partition;
        memset(e->name, 0, sizeof(e->name));
        memcpy(e->name, pt.name, sizeof(e->name) - 1);
    }
    *out_len = (size_t)n * sizeof(rtdbd_point_info_t);
}

/* ---- v3.9 T5：请求上下文 + 分发表 --------------------------------
 * 处理过程需要句柄、policy、audit、连接、连接表、分区白名单；原先
 * handle_request 用一串 `if (opcode==...)` 串起、参数逐层手传。这里把公共
 * 状态收进 rtdbd_ctx_t，各 opcode 处理体抽成 op_* 函数，由 kOps 表按
 * opcode 分发。**纯机械重构**：每个 op_* 的响应状态/负载/发送顺序与原实现
 * 逐字保持，对外契约不变；语义改动仅在新增分区维度处。 */
typedef struct {
    indurtdb_t*            h;       /* v3.9 句柄（分区感知 API 入口） */
    irt_policy_t*          policy;
    irt_audit_t*           audit;
    rtdbd_conn_t*          conn;    /* 当前连接（承载默认分区） */
    rtdbd_conn_t*          conns;   /* 连接表（广播/健康用） */
    int                    nconns;
    const rtdbd_conf_t*    conf;    /* v3.9 分区白名单（可为空配置） */
    uint32_t               max_subs;   /* v3.10：每连接订阅上限（--max-subs） */
    uint64_t               timeout_ns; /* v3.10：全局默认超时（--timeout-ms 换算） */
    const rtdbd_req_hdr_t* req;     /* 本次请求头（生命周期 = handle_request 栈） */
    rtdbd_resp_hdr_t       resp;    /* 本次响应头（magic/version 已置） */
} rtdbd_ctx_t;

/* 校验单个 point_id 是否可订阅。
 * 返回 0 表示可订阅；否则返回对应的状态码（NOT_FOUND/DENIED/BAD_REQUEST）。
 * 成功时通过输出参数返回 point 的 partition、uid 与点位快照。 */
static int point_subscribable(rtdbd_ctx_t* cx, uint32_t id,
                              uint32_t* out_partition, uint32_t* out_uid,
                              indurtdb_point_t* out_point)
{
    if (id >= g_max_points) return RTDBD_ST_NOT_FOUND;

    indurtdb_point_t pt;
    if (indurtdb_h_read_point(cx->h, id, &pt) != 0) return RTDBD_ST_NOT_FOUND;
    if (pt.name[0] == '\0') return RTDBD_ST_NOT_FOUND;

    uint32_t pid = 0, uid = 0;
    if (peer_cred(cx->conn->fd, &pid, &uid) != 0) return RTDBD_ST_INTERNAL;

    if (!irt_policy_allows(cx->policy, uid, pt.partition, id)) return RTDBD_ST_DENIED;
    if (!rtdbd_conf_partition_allowed(cx->conf, pt.partition)) return RTDBD_ST_DENIED;

    if (out_partition) *out_partition = pt.partition;
    if (out_uid) *out_uid = uid;
    if (out_point) *out_point = pt;
    return 0;
}

/* 替换连接的整个订阅集：清空旧 subs/subtab，按升序插入新 ids，
 * 并为每个 id 入队一帧当前值快照 NOTIFY。 */
static void conn_replace_subs(rtdbd_conn_t* c, const uint32_t* ids, uint32_t n,
                              indurtdb_t* h, uint32_t max_subs)
{
    c->nsubs = 0;
    rtdbd_subtab_clear(&c->sub);

    for (uint32_t i = 0; i < n; ++i) {
        uint32_t id = ids[i];
        if (c->nsubs >= max_subs || c->nsubs >= RTDBD_SUB_MAX) break;
        if (!rtdbd_subtab_insert(&c->sub, id)) continue;
        c->subs[c->nsubs++] = id;

        /* 构造当前值快照 NOTIFY */
        indurtdb_point_t pt;
        if (indurtdb_h_read_point(h, id, &pt) != 0) continue;
        if (pt.name[0] == '\0') continue;

        rtdbd_notify_t nt;
        memset(&nt, 0, sizeof(nt));
        nt.point_id     = id;
        nt.type         = pt.type;
        nt.reserved[0]  = pt.quality;  /* v3.10 T5：quality 放在 reserved[0] */
        /* 字符串值不走 notify（32B 放不下），value_bits 保持 0；
         * 监控台字符串点走 REST/GET。 */
        if (pt.type != RTDBD_TYPE_STRING)
            memcpy(&nt.value_bits, &pt.value, sizeof(nt.value_bits));
        nt.timestamp_ns   = pt.timestamp_ns;
        nt.source_ts_ns   = pt.source_timestamp_ns;

        outq_push(c, &nt);
    }
}

/* 处理器签名只带 fd 与上下文；请求头经 cx->req 取。这样分发签名里不会出现
 * 用不到的形参，也就无需以 (void) 消警（工程约定禁 (void) 消警）。 */
typedef int (*rtdbd_handler_fn)(int fd, rtdbd_ctx_t* cx);

static int op_ping(int fd, rtdbd_ctx_t* cx)
{
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

static int op_audit_dump(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_audit_entry_t out[RTDBD_AUDIT_CAPACITY];
    uint32_t n = irt_audit_dump(cx->audit, out, RTDBD_AUDIT_CAPACITY);

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = n * (uint32_t)sizeof(rtdbd_audit_entry_t);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    if (n > 0 && send_all(fd, out, cx->resp.payload_len) != 0) return 1;
    return 0;
}

static int op_write(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_write_req_t w;
    if (cx->req->payload_len != sizeof(w) || recv_all(fd, &w, sizeof(w)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    uint32_t pid = 0, uid = 0;
    if (peer_cred(fd, &pid, &uid) != 0) {
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* 鉴权：deny by default。分区取连接默认值（OP_SELECT 设置，T5）。 */
    uint32_t partition = cx->conn->partition;
    if (!irt_policy_allows(cx->policy, uid, partition, w.point_id)) {
        g_stats.write_rejected++;
        rtdbd_stats_note_error();
        cx->resp.status      = RTDBD_ST_DENIED;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    /* 白名单 fail-closed：连接默认分区不在白名单（如白名单 [1,2] 下初值 0）即拒写。 */
    if (!rtdbd_conf_partition_allowed(cx->conf, partition)) {
        g_stats.write_rejected++;
        rtdbd_stats_note_error();
        cx->resp.status      = RTDBD_ST_DENIED;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    indurtdb_meta_t m;
    int rc = do_write(cx->h, &w, &m);
    if (rc == -99) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc != 0) {
        g_stats.write_error++;
        rtdbd_stats_note_error();
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    irt_audit_record(cx->audit, pid, uid, partition, w.point_id, now_ns());
    g_stats.n_writes++;
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = 0;
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;

    /* 变更通知：写成功后广播给订阅者（含写者自身，若它也订阅了）；RBE 门控在内部 */
    notify_broadcast(cx->h, cx->conns, cx->nconns, &w, &m);
    return 0;
}

static int op_subscribe(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_sub_req_t s;
    if (cx->req->payload_len != sizeof(s) || recv_all(fd, &s, sizeof(s)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    if (cx->req->opcode == RTDBD_OP_SUBSCRIBE) {
        /* 校验：点位已注册、策略允许、分区白名单允许、未超 max_subs */
        int rc = point_subscribable(cx, s.point_id, NULL, NULL, NULL);
        if (rc != 0) {
            cx->resp.status      = (uint16_t)rc;
            cx->resp.payload_len = 0;
            return send_all(fd, &cx->resp, sizeof(cx->resp));
        }
        rc = conn_add_sub(cx->conn, s.point_id, cx->max_subs);
        cx->resp.status      = (rc == 0) ? RTDBD_ST_OK : RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    } else {
        /* UNSUBSCRIBE：无需校验，直接删除 */
        conn_del_sub(cx->conn, s.point_id);
        cx->resp.status      = RTDBD_ST_OK;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
}

/* ---- v3.4 T9：按名查找（读，无需鉴权）；v3.9：名字域为 (partition, name) ---- */
static int op_find_by_name(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_find_req_t f;
    if (cx->req->payload_len != sizeof(f) || recv_all(fd, &f, sizeof(f)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }
    f.name[sizeof(f.name) - 1] = '\0';

    /* 白名单 fail-closed：非法分区不存在该命名空间 ⇒ 未找到。 */
    if (!rtdbd_conf_partition_allowed(cx->conf, f.partition)) {
        cx->resp.status      = RTDBD_ST_NOT_FOUND;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    uint32_t found_id = 0;
    int rc = indurtdb_h_find_by_name_in(cx->h, f.partition, f.name, &found_id);
    if (rc == INDURTDB_ERR_NOT_FOUND) {
        cx->resp.status      = RTDBD_ST_NOT_FOUND;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc != 0) {
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    rtdbd_find_resp_t r;
    memset(&r, 0, sizeof(r));
    r.point_id = found_id;
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = (uint32_t)sizeof(r);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    return send_all(fd, &r, sizeof(r));
}

/* ---- v3.4 T9：读取元数据（读，无需鉴权） ---- */
static int op_get_meta(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_meta_req_t m;
    if (cx->req->payload_len != sizeof(m) || recv_all(fd, &m, sizeof(m)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    indurtdb_meta_t meta;
    memset(&meta, 0, sizeof(meta));
    int rc = indurtdb_h_get_meta(cx->h, m.point_id, &meta);
    if (rc == INDURTDB_ERR_NOT_FOUND) {
        cx->resp.status      = RTDBD_ST_NOT_FOUND;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc == INDURTDB_ERR_ARG) {   /* 越界 id 属请求错误，非"未找到" */
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc != 0) {
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = (uint32_t)sizeof(meta);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    return send_all(fd, &meta, sizeof(meta));
}

/* ---- v3.4 T9：写入元数据（管控写，须鉴权） ---- */
static int op_set_meta(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_set_meta_req_t s;
    if (cx->req->payload_len != sizeof(s) || recv_all(fd, &s, sizeof(s)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    uint32_t pid = 0, uid = 0;
    if (peer_cred(fd, &pid, &uid) != 0) {
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* 鉴权：deny by default；元数据写入属管控操作。分区取连接默认值（T5）。 */
    uint32_t partition = cx->conn->partition;
    if (!irt_policy_allows(cx->policy, uid, partition, s.point_id)) {
        cx->resp.status      = RTDBD_ST_DENIED;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (!rtdbd_conf_partition_allowed(cx->conf, partition)) {
        cx->resp.status      = RTDBD_ST_DENIED;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    indurtdb_meta_t m;
    memcpy(&m, &s.meta, sizeof(m)); /* 线结构 → 库结构（布局一致） */
    int rc = indurtdb_h_set_meta(cx->h, s.point_id, &m);
    if (rc == INDURTDB_ERR_NOT_FOUND) {
        cx->resp.status      = RTDBD_ST_NOT_FOUND;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc == INDURTDB_ERR_ARG) {   /* 越界 id 属请求错误，非"未找到" */
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (rc != 0) {
        cx->resp.status      = RTDBD_ST_INTERNAL;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    irt_audit_record(cx->audit, pid, uid, partition, s.point_id, now_ns());
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

/* ---- v3.6 管控写通道：点位 CRUD（管控写，须鉴权，deny by default）
 *      v3.9：以请求携带的 partition 为目标分区（(partition,name) 唯一键） ---- */
static int op_create_point(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_create_req_t c;
    if (cx->req->payload_len != sizeof(c) || recv_all(fd, &c, sizeof(c)) != 0) {
        cx->resp.status = RTDBD_ST_BAD_REQUEST; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    uint32_t pid = 0, uid = 0;
    if (peer_cred(fd, &pid, &uid) != 0) {
        cx->resp.status = RTDBD_ST_INTERNAL; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    if (!irt_policy_allows(cx->policy, uid, c.partition, c.point_id)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (!rtdbd_conf_partition_allowed(cx->conf, c.partition)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    c.name[sizeof(c.name) - 1] = '\0';
    int rc = indurtdb_h_create_point_in(cx->h, c.partition, c.point_id, c.name, c.type, c.access);
    if (rc == INDURTDB_ERR_ARG || rc == INDURTDB_ERR_FULL)
        cx->resp.status = RTDBD_ST_BAD_REQUEST;
    else if (rc == INDURTDB_ERR_NOT_FOUND)
        cx->resp.status = RTDBD_ST_NOT_FOUND;
    else if (rc != 0)
        cx->resp.status = RTDBD_ST_INTERNAL;
    else {
        irt_audit_record(cx->audit, pid, uid, c.partition, c.point_id, now_ns());
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO,
                        "create point id=%u name=%s partition=%u (uid=%u)",
                        c.point_id, c.name, c.partition, uid);
        cx->resp.status = RTDBD_ST_OK;
    }
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

static int op_delete_point(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_delete_req_t d;
    if (cx->req->payload_len != sizeof(d) || recv_all(fd, &d, sizeof(d)) != 0) {
        cx->resp.status = RTDBD_ST_BAD_REQUEST; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    uint32_t pid = 0, uid = 0;
    if (peer_cred(fd, &pid, &uid) != 0) {
        cx->resp.status = RTDBD_ST_INTERNAL; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    if (!irt_policy_allows(cx->policy, uid, d.partition, d.point_id)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (!rtdbd_conf_partition_allowed(cx->conf, d.partition)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    int rc = indurtdb_h_delete_point_in(cx->h, d.partition, d.point_id);
    if (rc == INDURTDB_ERR_NOT_FOUND)
        cx->resp.status = RTDBD_ST_NOT_FOUND;
    else if (rc == INDURTDB_ERR_ARG)
        cx->resp.status = RTDBD_ST_BAD_REQUEST;
    else if (rc != 0)
        cx->resp.status = RTDBD_ST_INTERNAL;
    else {
        irt_audit_record(cx->audit, pid, uid, d.partition, d.point_id, now_ns());
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO,
                        "delete point id=%u partition=%u (uid=%u)",
                        d.point_id, d.partition, uid);
        cx->resp.status = RTDBD_ST_OK;
    }
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

static int op_rename_point(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_rename_req_t r;
    if (cx->req->payload_len != sizeof(r) || recv_all(fd, &r, sizeof(r)) != 0) {
        cx->resp.status = RTDBD_ST_BAD_REQUEST; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    uint32_t pid = 0, uid = 0;
    if (peer_cred(fd, &pid, &uid) != 0) {
        cx->resp.status = RTDBD_ST_INTERNAL; cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp)); return 1;
    }
    if (!irt_policy_allows(cx->policy, uid, r.partition, r.point_id)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    if (!rtdbd_conf_partition_allowed(cx->conf, r.partition)) {
        cx->resp.status = RTDBD_ST_DENIED; cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    r.name[sizeof(r.name) - 1] = '\0';
    int rc = indurtdb_h_rename_point_in(cx->h, r.partition, r.point_id, r.name);
    if (rc == INDURTDB_ERR_NOT_FOUND)
        cx->resp.status = RTDBD_ST_NOT_FOUND;
    else if (rc == INDURTDB_ERR_ARG || rc == INDURTDB_ERR_FULL)
        cx->resp.status = RTDBD_ST_BAD_REQUEST;
    else if (rc != 0)
        cx->resp.status = RTDBD_ST_INTERNAL;
    else {
        irt_audit_record(cx->audit, pid, uid, r.partition, r.point_id, now_ns());
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO,
                        "rename point id=%u -> %s partition=%u (uid=%u)",
                        r.point_id, r.name, r.partition, uid);
        cx->resp.status = RTDBD_ST_OK;
    }
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

/* ---- v3.5 监控：读取单点当前值（读，无需鉴权） ---- */
static int op_get(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_get_req_t g;
    if (cx->req->payload_len != sizeof(g) || recv_all(fd, &g, sizeof(g)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    rtdbd_get_resp_t r;
    if (do_get(cx->h, g.point_id, &r) != 0) {
        cx->resp.status      = RTDBD_ST_NOT_FOUND; /* 越界/未注册均按未找到 */
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = (uint32_t)sizeof(r);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    return send_all(fd, &r, sizeof(r));
}

/* ---- v3.5 监控：枚举已注册点位（读，无需鉴权）；v3.9：按分区过滤 ---- */
static int op_list(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_list_req_t l;
    if (cx->req->payload_len != sizeof(l) || recv_all(fd, &l, sizeof(l)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* 白名单 fail-closed：具体分区非法 ⇒ 空结果；ALL 哨兵列出全部分区（不过滤）。 */
    if (l.partition != RTDBD_PARTITION_ALL &&
        !rtdbd_conf_partition_allowed(cx->conf, l.partition)) {
        cx->resp.status      = RTDBD_ST_OK;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }

    size_t len = 0;
    do_list(cx->h, l.partition, l.max, l.offset, g_list_buf, &len);
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = (uint32_t)len;
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    if (len > 0 && send_all(fd, g_list_buf, len) != 0) return 1;
    return 0;
}

/* ---- v3.6 运行日志（只读；与 AUDIT_DUMP 同类，无点位维度故不按 point 鉴权） ---- */
static int op_get_log(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_log_req_t q;
    if (cx->req->payload_len != sizeof(q) || recv_all(fd, &q, sizeof(q)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }
    uint32_t cap = (q.max == 0 || q.max > RTDBD_LOG_CAPACITY) ? RTDBD_LOG_CAPACITY : q.max;
    uint32_t n = irt_logbuf_dump(&g_logbuf, g_log_out, cap);

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = n * (uint32_t)sizeof(rtdbd_log_entry_t);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    if (n > 0 && send_all(fd, g_log_out, cx->resp.payload_len) != 0) return 1;
    return 0;
}

/* ---- v3.7 主题B B1：健康快照（只读、免鉴权，同 GET_LOG 定位） ---- */
static int op_health(int fd, rtdbd_ctx_t* cx)
{
    if (cx->req->payload_len != 0) {
        g_stats.decode_fail++;
        rtdbd_stats_note_error();
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* 返回前刷新自检：核心段头 + 错误计数分类 + 连接表完整性 */
    int health = rtdbd_self_check_core(cx->h);
    if (health != RTDBD_HEALTH_UNHEALTHY &&
        conn_table_check(cx->conns, cx->nconns) != 0) {
        health = RTDBD_HEALTH_UNHEALTHY;
        g_stats.health = RTDBD_HEALTH_UNHEALTHY;
    }

    rtdbd_health_t h;
    memset(&h, 0, sizeof(h));
    h.n_writes         = g_stats.n_writes;
    h.n_notifies       = g_stats.n_notifies;
    h.decode_fail      = g_stats.decode_fail;
    h.write_rejected   = g_stats.write_rejected;
    h.write_error      = g_stats.write_error;
    h.notify_drop      = g_stats.notify_drop;
    h.notify_send_fail = g_stats.notify_send_fail;
    h.uptime_ns        = g_stats.uptime_ns;
    h.health           = (uint32_t)health;
    uint32_t nc = 0;
    for (int i = 0; i < cx->nconns; ++i) if (cx->conns[i].fd >= 0) nc++;
    h.n_conns = nc;

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = (uint32_t)sizeof(h);
    if (send_all(fd, &cx->resp, sizeof(cx->resp)) != 0) return 1;
    return send_all(fd, &h, sizeof(h));
}

/* ---- v3.9 T5：选择连接默认分区（uint32 负载，白名单校验） ----
 * 成功：更新 conn->partition 并回 OK；分区非法：回 DENIED **不改**连接状态。
 * 负载长度错误同其它 op 的 BAD_REQUEST：回包后关闭连接（返回 1）。 */
static int op_select(int fd, rtdbd_ctx_t* cx)
{
    if (cx->req->payload_len != sizeof(uint32_t)) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }
    uint32_t p = 0;
    if (recv_all(fd, &p, sizeof(p)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }
    if (!rtdbd_conf_partition_allowed(cx->conf, p)) {
        cx->resp.status      = RTDBD_ST_DENIED;
        cx->resp.payload_len = 0;
        return send_all(fd, &cx->resp, sizeof(cx->resp));
    }
    cx->conn->partition = p;
    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

/* ---- v3.10 T4：SET_SUBS 批量订阅整组替换 + 当前值快照 ----
 * 请求：u32 count; u32 ids[count]
 * 响应：空负载，status=OK 表示成功
 * 成功后：清空出站队列（不计 notify_drop），替换订阅集，按 id 升序推送快照 NOTIFY
 * 失败：旧订阅集保持不变，返回对应错误码 */
static int op_set_subs(int fd, rtdbd_ctx_t* cx)
{
    /* 若出站队列有部分已发送帧，关闭连接而不回包：
     * 部分发送的 NOTIFY 帧在流中，插入响应会破坏帧边界。 */
    if (cx->conn->outq_sent != 0) return 1;

    /* 最小负载：4 字节 count；超过 4+4*1024 直接断连（无法安全 drain，
     * 也避免恶意超大 payload 让服务端空读）。 */
    if (cx->req->payload_len < 4 ||
        cx->req->payload_len > 4 + 4 * RTDBD_SET_SUBS_MAX) return 1;

    uint32_t count = 0;
    if (recv_all(fd, &count, sizeof(count)) != 0) {
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* count 超过单次 SET_SUBS 上限 */
    if (count > RTDBD_SET_SUBS_MAX) {
        /* 安全 drain 剩余 payload（最多 4*1024 字节） */
        uint8_t drain_buf[256];
        uint32_t remaining = cx->req->payload_len - 4;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(drain_buf) ? sizeof(drain_buf) : remaining;
            if (recv_all(fd, drain_buf, chunk) != 0) break;
            remaining -= chunk;
        }
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* payload_len 必须精确匹配 4 + 4*count */
    uint32_t expected_len = 4 + 4 * count;
    if (cx->req->payload_len != expected_len) {
        /* 安全 drain */
        uint8_t drain_buf[256];
        uint32_t remaining = cx->req->payload_len - 4;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(drain_buf) ? sizeof(drain_buf) : remaining;
            if (recv_all(fd, drain_buf, chunk) != 0) break;
            remaining -= chunk;
        }
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 1;
    }

    /* count 超过连接订阅上限 */
    if (count > cx->max_subs) {
        uint8_t drain_buf[256];
        uint32_t remaining = cx->req->payload_len - 4;
        while (remaining > 0) {
            uint32_t chunk = remaining > sizeof(drain_buf) ? sizeof(drain_buf) : remaining;
            if (recv_all(fd, drain_buf, chunk) != 0) break;
            remaining -= chunk;
        }
        cx->resp.status      = RTDBD_ST_BAD_REQUEST;
        cx->resp.payload_len = 0;
        (void)send_all(fd, &cx->resp, sizeof(cx->resp));
        return 0;  /* 保持连接，旧订阅不变 */
    }

    /* 读取所有 id 到静态缓冲 */
    for (uint32_t i = 0; i < count; ++i) {
        if (recv_all(fd, &g_set_subs_ids[i], sizeof(uint32_t)) != 0) {
            cx->resp.status      = RTDBD_ST_BAD_REQUEST;
            cx->resp.payload_len = 0;
            (void)send_all(fd, &cx->resp, sizeof(cx->resp));
            return 1;
        }
    }

    /* 校验重复 id（O(n^2)，n <= 1024 可接受） */
    for (uint32_t i = 0; i < count; ++i) {
        for (uint32_t j = i + 1; j < count; ++j) {
            if (g_set_subs_ids[i] == g_set_subs_ids[j]) {
                cx->resp.status      = RTDBD_ST_BAD_REQUEST;
                cx->resp.payload_len = 0;
                (void)send_all(fd, &cx->resp, sizeof(cx->resp));
                return 0;  /* 旧订阅不变 */
            }
        }
    }

    /* 逐个校验每个 id（注册/策略/分区白名单） */
    for (uint32_t i = 0; i < count; ++i) {
        uint32_t id = g_set_subs_ids[i];
        int rc = point_subscribable(cx, id, NULL, NULL, NULL);
        if (rc != 0) {
            cx->resp.status      = (uint16_t)rc;
            cx->resp.payload_len = 0;
            (void)send_all(fd, &cx->resp, sizeof(cx->resp));
            return 0;  /* 旧订阅不变 */
        }
    }

    /* 升序排序（插入排序，n <= 1024） */
    for (uint32_t i = 1; i < count; ++i) {
        uint32_t key = g_set_subs_ids[i];
        int32_t j = (int32_t)i - 1;
        while (j >= 0 && g_set_subs_ids[j] > key) {
            g_set_subs_ids[j + 1] = g_set_subs_ids[j];
            j--;
        }
        g_set_subs_ids[j + 1] = key;
    }

    /* 全部校验通过：软清理出站队列，替换订阅集并入队快照 */
    conn_clear_outq_soft(cx->conn);
    conn_replace_subs(cx->conn, g_set_subs_ids, count, cx->h, cx->max_subs);

    cx->resp.status      = RTDBD_ST_OK;
    cx->resp.payload_len = 0;
    return send_all(fd, &cx->resp, sizeof(cx->resp));
}

/* opcode → 处理器。查找失败 = 未知 opcode → BAD_REQUEST + 关连接（同 v2 行为）。 */
static const struct {
    uint16_t         op;
    rtdbd_handler_fn fn;
} kOps[] = {
    { RTDBD_OP_PING,         op_ping         },
    { RTDBD_OP_WRITE,        op_write        },
    { RTDBD_OP_SUBSCRIBE,    op_subscribe    },
    { RTDBD_OP_UNSUBSCRIBE,  op_subscribe    },   /* 同处理器内按 opcode 分派 */
    { RTDBD_OP_AUDIT_DUMP,   op_audit_dump   },
    { RTDBD_OP_FIND_BY_NAME, op_find_by_name },
    { RTDBD_OP_GET_META,     op_get_meta     },
    { RTDBD_OP_SET_META,     op_set_meta     },
    { RTDBD_OP_CREATE_POINT, op_create_point },
    { RTDBD_OP_DELETE_POINT, op_delete_point },
    { RTDBD_OP_RENAME_POINT, op_rename_point },
    { RTDBD_OP_GET,          op_get          },
    { RTDBD_OP_LIST,         op_list         },
    { RTDBD_OP_GET_LOG,      op_get_log      },
    { RTDBD_OP_HEALTH,       op_health       },
    { RTDBD_OP_SELECT,       op_select       },
    { RTDBD_OP_SET_SUBS,     op_set_subs     },
};

static int handle_request(int fd, rtdbd_ctx_t* cx)
{
    rtdbd_req_hdr_t req;
    if (recv_all(fd, &req, sizeof(req)) != 0) return 1;

    if (req.magic != RTDBD_MAGIC || req.version != RTDBD_PROTO_VERSION) {
        /* 版本不匹配：不静默，直接关闭连接 */
        g_stats.decode_fail++;
        rtdbd_stats_note_error();
        return 1;
    }

    cx->req = &req;
    memset(&cx->resp, 0, sizeof(cx->resp));
    cx->resp.magic   = RTDBD_MAGIC;
    cx->resp.version = RTDBD_PROTO_VERSION;

    for (size_t i = 0; i < sizeof(kOps) / sizeof(kOps[0]); ++i) {
        if (kOps[i].op == req.opcode) return kOps[i].fn(fd, cx);
    }

    cx->resp.status      = RTDBD_ST_BAD_REQUEST;
    cx->resp.payload_len = 0;
    (void)send_all(fd, &cx->resp, sizeof(cx->resp));
    return 1;
}

/* 写 worker pid 文件，供运维与测试观测当前 worker */
static void write_pidfile(const char* path, pid_t pid)
{
    if (!path) return;
    FILE* f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%d\n", (int)pid);
    fclose(f);
}

/* 终止并回收一个子进程：SIGTERM → 有界等待 → 超时 SIGKILL → 最终回收。
 *
 * 为什么不能只发 SIGTERM 就走：worker 可能正阻塞在 recv_all() 里，而
 * recv_all 的 EINTR 分支是 `continue` 重试 recv、并不检查 g_stop——
 * 此时 SIGTERM 只是打断了 recv，进程根本不会回到 poll 循环去看 g_stop，
 * 于是**永远不退出**而成孤儿（占着 shm 段、socket，且继承的 stdout/stderr
 * 管道写端不关闭，会把等 EOF 的调用方永久挂住）。故必须 SIGKILL 兜底。
 *
 * 返回 0 = 已确认回收；-1 = 仍无法回收（ECHILD 等，视为已不存在）。 */
static int terminate_and_reap(pid_t pid)
{
    if (pid <= 0) return -1;

    /* 先试 SIGTERM，给优雅退出（排空出站队列）的机会。
     * 轮询用指数退避而非固定 10ms×200：绝大多数 worker 会在几十毫秒内退出，
     * 固定 2s 预算会把每个"停止 supervisor"的动作都拖慢 2 秒（实测让
     * test_rtdbd_recovery 从 67ms 涨到 15s），故总预算压到 500ms。 */
    kill(pid, SIGTERM);
    {
        int waited_us = 0;
        int step_us   = 200;   /* 0.2ms → 1ms → 5ms → 10ms … */
        while (waited_us < 500000) {
            int st = 0;
            pid_t r = waitpid(pid, &st, WNOHANG);
            if (r == pid) return 0;                    /* 已回收 */
            if (r < 0 && errno == ECHILD) return -1;   /* 已不存在 */
            usleep((useconds_t)step_us);
            waited_us += step_us;
            if (step_us < 10000) step_us *= 5;
        }
    }

    /* 仍不退出（阻塞在 recv / 排空卡住）→ SIGKILL 强退并阻塞回收 */
    kill(pid, SIGKILL);
    for (;;) {
        int st = 0;
        pid_t r = waitpid(pid, &st, 0);
        if (r == pid) return 0;
        if (r < 0 && errno == EINTR) continue;
        return -1;                             /* ECHILD：已不存在 */
    }
}

/* ---- supervisor：worker 挂了立刻拉起，实现"马上起来继续运行"（T2） ----
 * 用 fork + execv 自身（去掉 --supervise）的方式拉起 worker，
 * 保证每次都是干净的进程，避免残留状态。
 */
static int supervise_loop(int argc, char** argv, const char* pidfile,
                          const char* sock_path)
{
    /* 不用 SA_RESTART：确保 SIGTERM 能打断 waitpid */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);   /* 仅置标志：避免 SIGUSR1 默认终止掉 supervisor */
    signal(SIGPIPE, SIG_IGN);

    /* 构造 worker 参数：去掉 --supervise / --pidfile */
    char* wargv[32];
    int   n = 0;
    wargv[n++] = (char*)"/proc/self/exe";
    for (int i = 1; i < argc && n < 31; ++i) {
        if (strcmp(argv[i], "--supervise") == 0) continue;
        if (strcmp(argv[i], "--pidfile") == 0) { i++; continue; }
        wargv[n++] = argv[i];
    }
    wargv[n] = NULL;

    irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO, "rtdbd supervisor started");

    /* cur：当前在管、未被 waitpid 回收的 worker。
     * 必须跨循环保留：EINTR 后 waitpid 未完成，此时若因 g_stop 跳出，
     * 仍须记得杀掉它 —— 否则留下孤儿进程，既占着共享内存段/socket，
     * 又因继承了父进程 stdout/stderr 管道写端而永不 EOF，
     * 会把 ctest 这类「等输出 EOF」的调用方永久挂住。 */
    pid_t cur = -1;

    for (;;) {
        if (g_stop) {
            if (cur > 0) { terminate_and_reap(cur); cur = -1; }
            break;
        }

        pid_t child = fork();
        if (child < 0) {
            usleep(10000);
            continue;
        }
        if (child == 0) {
            execv("/proc/self/exe", wargv);
            _exit(127);
        }

        cur = child;
        write_pidfile(pidfile, child);
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO, "rtdbd worker pid=%d", (int)child);

        /* 内层重试 waitpid：EINTR 只应重试本次等待，不能跳回 fork 层
         * （否则会重新 fork 并覆盖 cur，彻底失去对旧 worker 的追踪）。
         * 但 EINTR 往往正是 SIGTERM 置位 g_stop 造成的 —— 此时必须改为
         * 主动终止并回收，否则会一直阻塞在 waitpid 里，supervisor 永不退出
         * （实测表现为调用方超时后强杀 supervisor，worker 反成孤儿）。 */
        int status = 0;
        for (;;) {
            pid_t r = waitpid(child, &status, 0);
            if (r == child) break;          /* 已回收 */
            if (r < 0 && errno == EINTR) {
                if (g_stop) { terminate_and_reap(child); cur = -1; goto supervisor_done; }
                continue;                   /* 非停止信号：继续等待 */
            }
            /* waitpid 失败（含 ECHILD）：确认回收不了也要收拾掉 worker */
            terminate_and_reap(child);
            cur = -1;
            goto supervisor_done;
        }
        cur = -1;   /* 已回收 */

        if (g_stop) {
            /* 已回收场景下的 g_stop：无需再 kill，直接退出 */
            break;
        }

        /* 致命退出码（2=配置校验失败 / 4=启动自检失败）不 respawn：
         * 坏配置或损坏的段反复重启无意义，只会让系统陷入重启风暴。
         * 其余（含 3=shm 初始化失败，属可恢复的段占用/竞态）仍按原策略 respawn。 */
        if (WIFEXITED(status)) {
            int code = WEXITSTATUS(status);
            if (code == RTDBD_EXIT_CONFIG || code == RTDBD_EXIT_SELFCHECK) {
                irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                                "rtdbd: worker %d exited with fatal code %d, "
                                "not respawning", (int)child, code);
                irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                                "rtdbd: supervisor stopped due to fatal config/self-check");
                /* pidfile 与 socket 都要清：worker 是在 bind 之前退出的，
                 * 若只清 pidfile，前一轮遗留的 socket 会残留在文件系统上，
                 * 监控/测试可能仅凭 socket 存在就误判服务已启动。 */
                if (pidfile)    unlink(pidfile);
                if (sock_path)  unlink(sock_path);
                return code;   /* 透传给 systemd */
            }
        }

        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_WARN,
                        "rtdbd: worker %d exited, respawning", (int)child);
        usleep(1000); /* 极短退避，优先保证快速恢复 */
    }

supervisor_done:
    if (pidfile) unlink(pidfile);
    irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO, "rtdbd supervisor stopped");
    return 0;
}

/* 严格解析 CLI 数值参数：拒绝空串、负号、非数字、尾随字符与溢出/超 uint32。
 * 直接用 strtoul(optarg, NULL, 10) 会把 "-1" 变成 UINT32_MAX、把 "abc" 变 0。 */
static int parse_u32_cli(const char* s, uint32_t* out)
{
    if (!s || !out || s[0] == '\0') return -1;
    errno = 0;
    char* end = NULL;
    unsigned long v = strtoul(s, &end, 10);
    if (end == s || !end || *end != '\0') return -1;   /* 非数字 / 尾随字符 */
    if (errno == ERANGE || v > UINT32_MAX) return -1;  /* 溢出（含 "-1"） */
    *out = (uint32_t)v;
    return 0;
}

static void usage(FILE* out, const char* prog)
{
    fprintf(out,
            "Usage: %s [options]\n"
            "  -s, --socket PATH          UDS 监听路径（默认 /run/indurtdb/default.sock）\n"
            "  -i, --instance ID          实例 ID（共享内存段 /indurtdb_<ID>）\n"
            "  -p, --policy PATH          策略文件\n"
            "  -c, --config PATH          点位/分区配置 YAML\n"
            "  -m, --max-points N         点位容量（默认 10000）\n"
            "  -b, --max-subs N           每连接订阅上限（默认 1024，范围 1..1024）\n"
            "  -t, --timeout-ms N         全局默认点位超时毫秒（默认 0，0 = 不给全局默认）\n"
            "  -S, --supervise            以 supervisor 模式运行\n"
            "  -P, --pidfile PATH         supervisor 写入 worker pid 的文件\n"
            "  -F, --stats-file PATH      统计 dump 输出路径（默认 /tmp/rtdbd.stats）\n"
            "  -D, --delta-file PATH      运行时变更 delta 日志\n"
            "  -h, --help                 显示本帮助\n",
            prog ? prog : "rtdbd");
}

int main(int argc, char** argv)
{
    const char* sock_path  = "/run/indurtdb/default.sock";
    const char* instance   = "default";
    const char* policy_path = NULL;
    const char* config_path = NULL;
    uint32_t    max_points = 10000;
    uint32_t    max_subs   = RTDBD_SUB_MAX_PER_CONN;
    uint32_t    timeout_ms = 0;
    bool        supervise  = false;
    const char* pidfile    = NULL;
    /* v3.7 B4：运行时变更 delta 日志。**默认关闭**（未指定则不启用），
     * 以保持既有部署"运行时点位重启即丢"的语义不变。 */
    const char* delta_path = NULL;

    static struct option long_opts[] = {
        {"socket",     required_argument, 0, 's'},
        {"instance",   required_argument, 0, 'i'},
        {"policy",     required_argument, 0, 'p'},
        {"config",     required_argument, 0, 'c'},
        {"max-points", required_argument, 0, 'm'},
        {"max-subs",   required_argument, 0, 'b'},
        {"timeout-ms", required_argument, 0, 't'},
        {"supervise",  no_argument,       0, 'S'},
        {"pidfile",    required_argument, 0, 'P'},
        {"stats-file", required_argument, 0, 'F'},
        {"delta-file", required_argument, 0, 'D'},
        {"help",       no_argument,       0, 'h'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:i:p:c:m:b:t:SP:F:D:h", long_opts, NULL)) != -1) {
        switch (c) {
        case 's': sock_path = optarg; break;
        case 'i': instance = optarg; break;
        case 'p': policy_path = optarg; break;
        case 'c': config_path = optarg; break;
        case 'm':
            if (parse_u32_cli(optarg, &max_points) != 0) {
                fprintf(stderr, "rtdbd: invalid --max-points '%s'\n", optarg);
                return RTDBD_EXIT_ERROR;
            }
            break;
        case 'b':
            if (parse_u32_cli(optarg, &max_subs) != 0) {
                fprintf(stderr, "rtdbd: invalid --max-subs '%s'\n", optarg);
                return RTDBD_EXIT_ERROR;
            }
            break;
        case 't':
            if (parse_u32_cli(optarg, &timeout_ms) != 0) {
                fprintf(stderr, "rtdbd: invalid --timeout-ms '%s'\n", optarg);
                return RTDBD_EXIT_ERROR;
            }
            break;
        case 'S': supervise = true; break;
        case 'P': pidfile = optarg; break;
        case 'F':
            snprintf(g_stats_file, sizeof(g_stats_file), "%s", optarg);
            break;
        case 'D': delta_path = optarg; break;
        case 'h':
            usage(stdout, argv[0]);
            return RTDBD_EXIT_OK;
        default: break;
        }
    }

    g_max_points = max_points; /* 供监控 LIST 遍历点位表使用 */
    const uint64_t timeout_ns = (uint64_t)timeout_ms * 1000000ULL;
    irt_logbuf_init(&g_logbuf);
    rtdbd_stats_mark_start();   /* v3.7 B1：记录启动时刻，供 uptime 计算 */

    /* ---- B3 fail-fast 阶段 1/3：服务参数校验（在 initialize 之前，纯参数、不触 shm） ---- */
    if (!instance || instance[0] == '\0') {
        fprintf(stderr, "rtdbd: invalid --instance (empty)\n");
        return RTDBD_EXIT_ERROR;
    }
    {
        /* instance 会进入共享内存段名 /indurtdb_<id>，须为安全字符集 */
        for (const char* q = instance; *q; ++q) {
            int okc = (*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                      (*q >= '0' && *q <= '9') || *q == '_' || *q == '-';
            if (!okc) {
                fprintf(stderr,
                        "rtdbd: invalid --instance '%s' (allowed: [A-Za-z0-9_-])\n", instance);
                return RTDBD_EXIT_ERROR;
            }
        }
    }
    /* --max-points 上界：段布局偏移存在 uint32 字段，且 mmap/ftruncate 规模
     * 直接由点数决定。给一个远大于实用（默认 10000）但不会溢出布局的硬上限，
     * 挡住 --max-points -1（会变 UINT32_MAX）这类输入。 */
    if (max_points == 0 || max_points > RTDBD_MAX_POINTS_LIMIT) {
        fprintf(stderr, "rtdbd: invalid --max-points (must be in [1,%u])\n",
                (unsigned)RTDBD_MAX_POINTS_LIMIT);
        return RTDBD_EXIT_ERROR;
    }
    if (max_subs == 0 || max_subs > RTDBD_SUB_MAX_PER_CONN) {
        fprintf(stderr, "rtdbd: invalid --max-subs (must be in [1,%u])\n",
                (unsigned)RTDBD_SUB_MAX_PER_CONN);
        return RTDBD_EXIT_CONFIG;
    }
    if (!sock_path || sock_path[0] == '\0' ||
        strlen(sock_path) >= sizeof(((struct sockaddr_un*)0)->sun_path)) {
        fprintf(stderr, "rtdbd: invalid --socket (empty or too long)\n");
        return RTDBD_EXIT_ERROR;
    }

    if (supervise) {
        return supervise_loop(argc, argv, pidfile, sock_path);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGUSR1, on_signal);   /* v3.7 B1：请求 dump 运行统计 */

    irt_policy_t policy;
    irt_policy_init(&policy);
    if (policy_path) {
        uint32_t pline = 0;
        int      prc   = irt_policy_load(&policy, policy_path, &pline);
        if (prc != IRT_POLICY_OK) {
            if (pline > 0) {
                irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                                "rtdbd: invalid policy %s: line %u: %s",
                                policy_path, pline, irt_policy_strerror(prc));
            } else {
                irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                                "rtdbd: failed to load policy %s: %s",
                                policy_path, irt_policy_strerror(prc));
            }
            /* 策略属配置：以配置校验退出码终止，supervisor/systemd 才会
             * 停止 respawn（否则坏策略会触发无限重启循环）。 */
            return RTDBD_EXIT_CONFIG;
        }
    }

    irt_audit_t audit;
    irt_audit_init(&audit);

    /* ---- v3.9 T5：解析分区白名单/别名（与点位同文件的 `points:` 之前键）。
     * 与 policy / 点位段同一 fail-fast 约定：格式错误 → exit 2（保留段）。 ---- */
    rtdbd_conf_t conf;
    if (config_path) {
        uint32_t cline = 0;
        if (rtdbd_conf_load(&conf, config_path, &cline) != 0) {
            fprintf(stderr, "rtdbd: invalid config (partition) '%s': line %u\n",
                    config_path, (unsigned)cline);
            irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                            "rtdbd: partition config invalid at line %u", (unsigned)cline);
            return RTDBD_EXIT_CONFIG;
        }
    } else {
        rtdbd_conf_init(&conf);
    }

    /* v3.9 T5：改用句柄 API 打开实例（分区感知 API 只存在于句柄层）。 */
    indurtdb_cfg_t cfg;
    cfg.max_points      = max_points;
    /* v3.10 D7：段订阅者槽位固定 RTDBD_MAX_CLIENTS，与 --max-subs（每连接上限）无关。 */
    cfg.max_subscribers = RTDBD_MAX_CLIENTS;
    if (indurtdb_h_open(&g_h, instance, &cfg) != 0) {
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                        "rtdbd: indurtdb_h_open failed: %s", indurtdb_get_last_error());
        return RTDBD_EXIT_SHM;
    }

    /* ---- B3 fail-fast 阶段 2/3：点位配置校验 ----
     * load_config 失败 → exit 2。
     * 校验严格性分档（向后兼容关键）：
     *   - 给了 --config：本次启动显式应用的语义必须合法，非法即 exit 2；
     *   - 未给 --config：段内 meta 来自历史残留（可能是 v3.6 时代经 SET_META
     *     写入的、当时合法的值），只告警不致命，避免旧部署被"砖化"。 */
    if (config_path && indurtdb_h_load_config(g_h, config_path) != 0) {
        fprintf(stderr, "rtdbd: invalid config '%s': %s\n",
                config_path, indurtdb_get_last_error());
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                        "rtdbd: load config failed: %s", indurtdb_get_last_error());
        /* 保留段：**不调用** indurtdb_h_close —— 它作为 owner 会 shm_unlink 段；
         * 本进程无 detach，直接返回即可（退出即 unmap，段与点位/值原样保留）。 */
        return RTDBD_EXIT_CONFIG;
    }

    /* ---- B4：回放 delta（在 base 之上叠加运行时变更），随后启用后续追加 ----
     * 顺序**必须**是 load_config(base) → replay(delta) → 校验 → 服务：
     * load_config 会无条件覆盖槽位，若先回放再 load_config，base 会把运行时
     * DELETE 掉的点"复活"、把 RENAME 的结果覆盖回原名 —— B4 的承诺对 base
     * 配置点（最常见的操作对象）完全失效。 */
    if (delta_path) {
        int applied = indurtdb_h_delta_replay(g_h, delta_path);
        if (applied < 0) {
            fprintf(stderr, "rtdbd: delta replay failed for '%s': %s\n",
                    delta_path, indurtdb_get_last_error());
            irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                            "rtdbd: delta replay failed: %s", indurtdb_get_last_error());
            return RTDBD_EXIT_CONFIG;   /* 保留段（不 close，避免 unlink） */
        }
        if (indurtdb_h_enable_delta(g_h, delta_path) != INDURTDB_OK) {
            fprintf(stderr, "rtdbd: cannot open delta file '%s': %s\n",
                    delta_path, indurtdb_get_last_error());
            irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                            "rtdbd: delta enable failed: %s", indurtdb_get_last_error());
            return RTDBD_EXIT_CONFIG;   /* 保留段（不 close，避免 unlink） */
        }
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO,
                        "rtdbd: delta '%s' replayed %d record(s)", delta_path, applied);
    }

    {
        uint32_t bad = 0;
        uint32_t vfield = 0;
        const char* vreason = NULL;
        int verr = rtdbd_validate_config(g_h, &bad, &vfield, &vreason);
        if (verr != INDURTDB_CFG_OK && config_path) {
            fprintf(stderr,
                    "rtdbd: invalid point config: point %u: %s (field=%u)\n",
                    (unsigned)bad,
                    vreason ? vreason : "invalid", (unsigned)vfield);
            irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                            "rtdbd: config invalid at point %u: %s",
                            (unsigned)bad, vreason ? vreason : "invalid");
            return RTDBD_EXIT_CONFIG;   /* 保留段（不 close，避免 unlink） */
        }
        if (bad != 0) {
            /* 残留段：告警继续服务，避免历史部署无法启动 */
            irt_logbuf_emit(&g_logbuf, RTDBD_LOG_WARN,
                            "rtdbd: legacy segment has invalid meta at point %u: %s "
                            "(continuing; fix config or clear segment to remove)",
                            (unsigned)bad, vreason ? vreason : "invalid");
        }

        /* 非致命提示：百分比死区但量程跨度无效 → 阈值退化为 0，死区永不触发。
         * 刻意只告警不拒绝：百分比死区与 EUR 位正交，配置本身合法。 */
        for (uint32_t id = 0; id < g_max_points; ++id) {
            indurtdb_point_t pt;
            if (indurtdb_h_read_point(g_h, id, &pt) != 0 || pt.name[0] == '\0') continue;
            indurtdb_meta_t m;
            memset(&m, 0, sizeof(m));
            (void)indurtdb_h_get_meta(g_h, id, &m);
            if (indurtdb_meta_pct_without_range(&m)) {
                irt_logbuf_emit(&g_logbuf, RTDBD_LOG_WARN,
                                "rtdbd: point %u has percent deadband but invalid EURange "
                                "span (eur_max <= eur_min): deadband never triggers",
                                (unsigned)id);
            }
        }
    }

    /* ---- B3 fail-fast 阶段 3/3：启动自检（段头 magic/version） ---- */
    if (indurtdb_h_self_check(g_h) != INDURTDB_HEALTH_OK) {
        fprintf(stderr, "rtdbd: self-check failed: %s\n", indurtdb_get_last_error());
        irt_logbuf_emit(&g_logbuf, RTDBD_LOG_ERROR,
                        "rtdbd: self-check failed: %s", indurtdb_get_last_error());
        return RTDBD_EXIT_SELFCHECK;   /* 保留段（不 close，避免 unlink） */
    }

    unlink(sock_path);

    int listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (listen_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);

    if (bind(listen_fd, (struct sockaddr*)&addr, sizeof(addr)) < 0 || listen(listen_fd, 8) < 0) {
        perror("bind/listen");
        close(listen_fd);
        return RTDBD_EXIT_ERROR;
    }

    irt_logbuf_emit(&g_logbuf, RTDBD_LOG_INFO,
                    "rtdbd listening on %s (instance=%s)", sock_path, instance);

    rtdbd_conn_t* conns = g_conns;   /* 指向文件域全局表（避免占用栈帧） */
    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) conn_init(&conns[i]);
    while (!g_stop) {
        struct pollfd fds[RTDBD_MAX_CLIENTS + 1];
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        int nfds = 1;

        for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
            if (conns[i].fd >= 0) {
                fds[nfds].fd = conns[i].fd;
                /* B2 背压：有待发通知时才监听 POLLOUT；否则纯 POLLIN。
                 * 慢消费者 socket 缓冲满 → POLLOUT 不就绪 → 本连接不被 flush，
                 * 不影响其他连接（消除 head-of-line 阻塞）。 */
                fds[nfds].events = (conns[i].outq_count > 0)
                                     ? (short)(POLLIN | POLLOUT)
                                     : POLLIN;
                nfds++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
        }

        /* SIGUSR1：dump 运行统计到 g_stats_file（含连接摘要）。
         * 在主循环内执行（非信号上下文），自检后落盘。 */
        if (g_dump_stats) {
            g_dump_stats = 0;
            if (rtdbd_self_check() != RTDBD_HEALTH_UNHEALTHY &&
                conn_table_check(conns, RTDBD_MAX_CLIENTS) != 0) {
                g_stats.health = RTDBD_HEALTH_UNHEALTHY;
            }
            FILE* fp = fopen(g_stats_file, "w");
            if (fp) {
                uint32_t nc = 0;
                for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i)
                    if (conns[i].fd >= 0) nc++;
                fprintf(fp, "n_conns          = %u\n", nc);
                rtdbd_stats_dump(fp);
                fclose(fp);
            }
        }

        if (fds[0].revents & POLLIN) {
            int cfd = accept(listen_fd, NULL, NULL);
            if (cfd >= 0) {
                int placed = 0;
                for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
                    if (conns[i].fd < 0) {
                        conn_init(&conns[i]);
                        conns[i].fd = cfd;
                        placed = 1;
                        break;
                    }
                }
                if (!placed) close(cfd);
            }
        }

        for (int i = 1; i < nfds; ++i) {
            rtdbd_conn_t* conn = NULL;
            for (int k = 0; k < RTDBD_MAX_CLIENTS; ++k) {
                if (conns[k].fd == fds[i].fd) { conn = &conns[k]; break; }
            }
            if (!conn) continue;

            /* B2：先 flush 出站队列（仅 POLLOUT 就绪时），再处理请求。
             * flush 失败（对端已关闭）→ 关闭该连接。 */
            if ((fds[i].revents & POLLOUT) && conn->outq_count > 0) {
                if (outq_flush(conn) != 0) {
                    close(fds[i].fd);
                    conn_init(conn);
                    continue;
                }
            }

            if (!(fds[i].revents & POLLIN)) continue;

            rtdbd_ctx_t cx;
            cx.h          = g_h;
            cx.policy     = &policy;
            cx.audit      = &audit;
            cx.conn       = conn;
            cx.conns      = conns;
            cx.nconns     = RTDBD_MAX_CLIENTS;
            cx.conf       = &conf;
            cx.max_subs   = max_subs;
            cx.timeout_ns = timeout_ns;
            cx.req        = NULL;
            memset(&cx.resp, 0, sizeof(cx.resp));

            if (handle_request(fds[i].fd, &cx) != 0) {
                close(fds[i].fd);
                conn_discard_outq(conn);   /* 队列内容计入 notify_drop，不静默丢 */
                conn_init(conn);
            }
        }
    }

    /* ---- B2 优雅退出：限时 best-effort 排空所有出站队列 ----
     * 停收新连接（listen 已不再 poll）→ 把待发通知尽量送完再关 fd。
     * 受 RTDBD_SHUTDOWN_DELAY_SEC 限时：超时则放弃剩余（不阻塞强退）。 */
    {
        int64_t deadline_ms = (int64_t)RTDBD_SHUTDOWN_DELAY_SEC * 1000;
        while (deadline_ms > 0) {
            struct pollfd pfd[RTDBD_MAX_CLIENTS];
            int np = 0;
            for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
                if (conns[i].fd >= 0 && conns[i].outq_count > 0) {
                    pfd[np].fd = conns[i].fd;
                    pfd[np].events = POLLOUT;
                    pfd[np].revents = 0;
                    np++;
                }
            }
            if (np == 0) break;   /* 全部排空 */

            int slice = (deadline_ms < 100) ? (int)deadline_ms : 100;
            int pr = poll(pfd, (nfds_t)np, slice);
            if (pr < 0) {
                if (errno == EINTR) { deadline_ms -= slice; continue; }
                break;
            }
            deadline_ms -= slice;
            if (pr == 0) continue;   /* 本轮无就绪，继续等（受 deadline 约束） */

            for (int i = 0; i < np; ++i) {
                if (!(pfd[i].revents & POLLOUT)) continue;
                for (int k = 0; k < RTDBD_MAX_CLIENTS; ++k) {
                    if (conns[k].fd == pfd[i].fd && conns[k].outq_count > 0) {
                        (void)outq_flush(&conns[k]);   /* 失败已计 notify_send_fail */
                        break;
                    }
                }
            }
        }
    }

    /* 排空超时仍未发完的：统一计入 notify_drop，不静默丢弃 */
    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) conn_discard_outq(&conns[i]);

    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
        if (conns[i].fd >= 0) close(conns[i].fd);
    }
    close(listen_fd);
    unlink(sock_path);

    indurtdb_h_close(g_h);
    return RTDBD_EXIT_OK;
}
