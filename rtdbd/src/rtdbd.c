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

/* 线结构 rtdbd_meta_payload_t 与库结构 indurtdb_meta_t 必须逐字节同尺寸，
 * 否则 rtdbd.c 的 memcpy(&m, &s.meta, sizeof(m)) 会越界/截断（protocol.h:108）。 */
RTDBD_STATIC_ASSERT(sizeof(rtdbd_meta_payload_t) == sizeof(indurtdb_meta_t),
               "rtdbd_meta_payload_t must match indurtdb_meta_t size");

#define RTDBD_MAX_CLIENTS 32
#define RTDBD_SHUTDOWN_DELAY_SEC 1

static volatile sig_atomic_t g_stop = 0;

/* 监控 LIST 用的实例点位上限与分片缓冲（rtdbd 串行处理，静态缓冲安全） */
static uint32_t g_max_points = 0;
#define RTDBD_LIST_CHUNK 1024u
static uint8_t g_list_buf[RTDBD_LIST_CHUNK * sizeof(rtdbd_point_info_t)];

static void on_signal(int sig)
{
    (void)sig;
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

/* ---- 连接状态：每连接的订阅表（定长，无堆分配） ---- */
typedef struct {
    int      fd;
    uint32_t subs[RTDBD_SUB_MAX];
    uint32_t nsubs;
} rtdbd_conn_t;

static void conn_init(rtdbd_conn_t* c)
{
    c->fd = -1;
    c->nsubs = 0;
}

static bool conn_has_sub(const rtdbd_conn_t* c, uint32_t point_id)
{
    for (uint32_t i = 0; i < c->nsubs; ++i) {
        if (c->subs[i] == point_id) return true;
    }
    return false;
}

static int conn_add_sub(rtdbd_conn_t* c, uint32_t point_id)
{
    if (conn_has_sub(c, point_id)) return 0;
    if (c->nsubs >= RTDBD_SUB_MAX) return -1;
    c->subs[c->nsubs++] = point_id;
    return 0;
}

static void conn_del_sub(rtdbd_conn_t* c, uint32_t point_id)
{
    for (uint32_t i = 0; i < c->nsubs; ++i) {
        if (c->subs[i] != point_id) continue;
        c->subs[i] = c->subs[c->nsubs - 1];
        c->nsubs--;
        return;
    }
}

/* 写成功后向所有订阅了该点位的连接广播变更通知（T4） */
static void notify_broadcast(rtdbd_conn_t* conns, int n, const rtdbd_write_req_t* w)
{
    rtdbd_req_hdr_t  nh;
    rtdbd_notify_t   nt;

    memset(&nh, 0, sizeof(nh));
    memset(&nt, 0, sizeof(nt));

    nh.magic       = RTDBD_MAGIC;
    nh.version     = RTDBD_PROTO_VERSION;
    nh.opcode      = RTDBD_OP_NOTIFY;
    nh.payload_len = (uint32_t)sizeof(nt);

    nt.point_id     = w->point_id;
    nt.type         = w->type;
    nt.value_bits   = w->value_bits;
    nt.source_ts_ns = w->source_ts_ns;
    nt.timestamp_ns = 0; /* 由服务端在广播前补齐 */

    for (int i = 0; i < n; ++i) {
        rtdbd_conn_t* c = &conns[i];
        if (c->fd < 0) continue;
        if (!conn_has_sub(c, w->point_id)) continue;

        /* 取当前值的时间戳，保证通知携带最新入库时刻 */
        indurtdb_point_t pt;
        if (indurtdb_read_point(w->point_id, &pt) == 0) {
            nt.timestamp_ns = pt.timestamp_ns;
        }

        if (send_all(c->fd, &nh, sizeof(nh)) != 0 ||
            send_all(c->fd, &nt, sizeof(nt)) != 0) {
            /* 推送失败（对端已关闭）：移除该订阅，避免反复失败 */
            conn_del_sub(c, w->point_id);
        }
    }
}

/* 取对端进程凭据（pid / uid），用于鉴权与审计 */
static int peer_cred(int fd, uint32_t* pid, uint32_t* uid)
{
    struct ucred cr;
    socklen_t    len = sizeof(cr);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &cr, &len) != 0) return -1;
    *pid = (uint32_t)cr.pid;
    *uid = (uint32_t)cr.uid;
    return 0;
}

/* 执行一次点位写入（携带采集时刻）。返回 0 成功，非 0 失败
 *
 * 采集时刻由客户端经协议传入；为 0 表示"未提供"，
 * 库侧语义退化为仅记录入库时刻（与旧行为一致）。
 */
static int do_write(const rtdbd_write_req_t* w)
{
    switch (w->type) {
    case RTDBD_TYPE_BOOL:
        return indurtdb_write_bool_ts(w->point_id, (w->value_bits & 1u) ? true : false,
                                      w->source_ts_ns);
    case RTDBD_TYPE_INT32:
        return indurtdb_write_int32_ts(w->point_id, (int32_t)w->value_bits,
                                       w->source_ts_ns);
    case RTDBD_TYPE_INT64: {
        int64_t v = 0;
        memcpy(&v, &w->value_bits, sizeof(v));
        return indurtdb_write_int64_ts(w->point_id, v, w->source_ts_ns);
    }
    case RTDBD_TYPE_UINT32:
        return indurtdb_write_uint32_ts(w->point_id, (uint32_t)w->value_bits,
                                        w->source_ts_ns);
    case RTDBD_TYPE_FLOAT: {
        float f = 0.0f;
        memcpy(&f, &w->value_bits, sizeof(f));
        return indurtdb_write_float_ts(w->point_id, f, w->source_ts_ns);
    }
    case RTDBD_TYPE_DOUBLE: {
        double d = 0.0;
        memcpy(&d, &w->value_bits, sizeof(d));
        return indurtdb_write_double_ts(w->point_id, d, w->source_ts_ns);
    }
    default:
        return -99; /* 不支持的类型（如 STRING 受 8B 负载限制） */
    }
}

/* 返回 0 表示连接可继续保持；非 0 表示需关闭连接 */
/* ---- v3.5 监控只读通道：GET / LIST（读免鉴权） ---- */

/* 读取单点当前值快照；成功返回 0，失败（id 无效/未注册）返回 -1 */
static int do_get(uint32_t id, rtdbd_get_resp_t* out)
{
    indurtdb_point_t pt;
    if (indurtdb_read_point(id, &pt) != 0) return -1;
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
 * 结果写入 buf，*out_len 返回字节数。分页时 offset 为起始 id，max 为返回条数上限。 */
static void do_list(uint32_t max_n, uint32_t offset, uint8_t* buf, size_t* out_len)
{
    *out_len = 0;
    if (offset >= g_max_points) return;

    uint32_t cap = (max_n == 0) ? RTDBD_LIST_CHUNK : max_n;
    if (cap > RTDBD_LIST_CHUNK) cap = RTDBD_LIST_CHUNK;

    rtdbd_point_info_t* arr = (rtdbd_point_info_t*)buf;
    uint32_t n = 0;
    for (uint32_t id = offset; id < g_max_points && n < cap; ++id) {
        indurtdb_point_t pt;
        if (indurtdb_read_point(id, &pt) != 0) continue;
        if (pt.name[0] == '\0') continue; /* 未注册点位跳过 */

        rtdbd_point_info_t* e = &arr[n++];
        e->point_id = id;
        e->type     = pt.type;
        e->access   = pt.access;
        memset(e->name, 0, sizeof(e->name));
        memcpy(e->name, pt.name, sizeof(e->name) - 1);
    }
    *out_len = (size_t)n * sizeof(rtdbd_point_info_t);
}

static int handle_request(int fd, irt_policy_t* policy, irt_audit_t* audit,
                          rtdbd_conn_t* conn, rtdbd_conn_t* conns, int nconns)
{
    rtdbd_req_hdr_t req;
    if (recv_all(fd, &req, sizeof(req)) != 0) return 1;

    if (req.magic != RTDBD_MAGIC || req.version != RTDBD_PROTO_VERSION) {
        /* 版本不匹配：不静默，直接关闭连接 */
        return 1;
    }

    rtdbd_resp_hdr_t resp;
    memset(&resp, 0, sizeof(resp));
    resp.magic   = RTDBD_MAGIC;
    resp.version = RTDBD_PROTO_VERSION;

    if (req.opcode == RTDBD_OP_PING) {
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = 0;
        return send_all(fd, &resp, sizeof(resp));
    }

    if (req.opcode == RTDBD_OP_AUDIT_DUMP) {
        rtdbd_audit_entry_t out[RTDBD_AUDIT_CAPACITY];
        uint32_t n = irt_audit_dump(audit, out, RTDBD_AUDIT_CAPACITY);

        resp.status      = RTDBD_ST_OK;
        resp.payload_len = n * (uint32_t)sizeof(rtdbd_audit_entry_t);
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        if (n > 0 && send_all(fd, out, resp.payload_len) != 0) return 1;
        return 0;
    }

    if (req.opcode == RTDBD_OP_WRITE) {
        rtdbd_write_req_t w;
        if (req.payload_len != sizeof(w) || recv_all(fd, &w, sizeof(w)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        uint32_t pid = 0, uid = 0;
        if (peer_cred(fd, &pid, &uid) != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        /* 鉴权：deny by default */
        if (!irt_policy_allows(policy, uid, w.point_id)) {
            resp.status      = RTDBD_ST_DENIED;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        int rc = do_write(&w);
        if (rc == -99) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        irt_audit_record(audit, pid, uid, w.point_id, now_ns());
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = 0;
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;

        /* 变更通知：写成功后广播给订阅者（含写者自身，若它也订阅了） */
        notify_broadcast(conns, nconns, &w);
        return 0;
    }

    if (req.opcode == RTDBD_OP_SUBSCRIBE || req.opcode == RTDBD_OP_UNSUBSCRIBE) {
        rtdbd_sub_req_t s;
        if (req.payload_len != sizeof(s) || recv_all(fd, &s, sizeof(s)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }
        int rc = (req.opcode == RTDBD_OP_SUBSCRIBE)
                     ? conn_add_sub(conn, s.point_id)
                     : (conn_del_sub(conn, s.point_id), 0);

        resp.status      = (rc == 0) ? RTDBD_ST_OK : RTDBD_ST_INTERNAL;
        resp.payload_len = 0;
        return send_all(fd, &resp, sizeof(resp));
    }

    /* ---- v3.4 T9：按名查找（读，无需鉴权） ---- */
    if (req.opcode == RTDBD_OP_FIND_BY_NAME) {
        rtdbd_find_req_t f;
        if (req.payload_len != sizeof(f) || recv_all(fd, &f, sizeof(f)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }
        f.name[sizeof(f.name) - 1] = '\0';

        uint32_t found_id = 0;
        int rc = indurtdb_find_by_name(f.name, &found_id);
        if (rc == INDURTDB_ERR_NOT_FOUND) {
            resp.status      = RTDBD_ST_NOT_FOUND;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        rtdbd_find_resp_t r;
        memset(&r, 0, sizeof(r));
        r.point_id = found_id;
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = (uint32_t)sizeof(r);
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        return send_all(fd, &r, sizeof(r));
    }

    /* ---- v3.4 T9：读取元数据（读，无需鉴权） ---- */
    if (req.opcode == RTDBD_OP_GET_META) {
        rtdbd_meta_req_t m;
        if (req.payload_len != sizeof(m) || recv_all(fd, &m, sizeof(m)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        indurtdb_meta_t meta;
        memset(&meta, 0, sizeof(meta));
        int rc = indurtdb_get_meta(m.point_id, &meta);
        if (rc == INDURTDB_ERR_NOT_FOUND) {
            resp.status      = RTDBD_ST_NOT_FOUND;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc == INDURTDB_ERR_ARG) {   /* 越界 id 属请求错误，非"未找到" */
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        resp.status      = RTDBD_ST_OK;
        resp.payload_len = (uint32_t)sizeof(meta);
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        return send_all(fd, &meta, sizeof(meta));
    }

    /* ---- v3.4 T9：写入元数据（管控写，须鉴权） ---- */
    if (req.opcode == RTDBD_OP_SET_META) {
        rtdbd_set_meta_req_t s;
        if (req.payload_len != sizeof(s) || recv_all(fd, &s, sizeof(s)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        uint32_t pid = 0, uid = 0;
        if (peer_cred(fd, &pid, &uid) != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        /* 鉴权：deny by default；元数据写入属管控操作 */
        if (!irt_policy_allows(policy, uid, s.point_id)) {
            resp.status      = RTDBD_ST_DENIED;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        indurtdb_meta_t m;
        memcpy(&m, &s.meta, sizeof(m)); /* 线结构 → 库结构（布局一致） */
        int rc = indurtdb_set_meta(s.point_id, &m);
        if (rc == INDURTDB_ERR_NOT_FOUND) {
            resp.status      = RTDBD_ST_NOT_FOUND;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc == INDURTDB_ERR_ARG) {   /* 越界 id 属请求错误，非"未找到" */
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }
        if (rc != 0) {
            resp.status      = RTDBD_ST_INTERNAL;
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        irt_audit_record(audit, pid, uid, s.point_id, now_ns());
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = 0;
        return send_all(fd, &resp, sizeof(resp));
    }

    /* ---- v3.5 监控：读取单点当前值（读，无需鉴权） ---- */
    if (req.opcode == RTDBD_OP_GET) {
        rtdbd_get_req_t g;
        if (req.payload_len != sizeof(g) || recv_all(fd, &g, sizeof(g)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        rtdbd_get_resp_t r;
        if (do_get(g.point_id, &r) != 0) {
            resp.status      = RTDBD_ST_NOT_FOUND; /* 越界/未注册均按未找到 */
            resp.payload_len = 0;
            return send_all(fd, &resp, sizeof(resp));
        }

        resp.status      = RTDBD_ST_OK;
        resp.payload_len = (uint32_t)sizeof(r);
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        return send_all(fd, &r, sizeof(r));
    }

    /* ---- v3.5 监控：枚举已注册点位（读，无需鉴权） ---- */
    if (req.opcode == RTDBD_OP_LIST) {
        rtdbd_list_req_t l;
        if (req.payload_len != sizeof(l) || recv_all(fd, &l, sizeof(l)) != 0) {
            resp.status      = RTDBD_ST_BAD_REQUEST;
            resp.payload_len = 0;
            (void)send_all(fd, &resp, sizeof(resp));
            return 1;
        }

        size_t len = 0;
        do_list(l.max, l.offset, g_list_buf, &len);
        resp.status      = RTDBD_ST_OK;
        resp.payload_len = (uint32_t)len;
        if (send_all(fd, &resp, sizeof(resp)) != 0) return 1;
        if (len > 0 && send_all(fd, g_list_buf, len) != 0) return 1;
        return 0;
    }

    resp.status      = RTDBD_ST_BAD_REQUEST;
    resp.payload_len = 0;
    (void)send_all(fd, &resp, sizeof(resp));
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

/* ---- supervisor：worker 挂了立刻拉起，实现"马上起来继续运行"（T2） ----
 * 用 fork + execv 自身（去掉 --supervise）的方式拉起 worker，
 * 保证每次都是干净的进程，避免残留状态。
 */
static int supervise_loop(int argc, char** argv, const char* pidfile)
{
    /* 不用 SA_RESTART：确保 SIGTERM 能打断 waitpid */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_signal;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
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

    printf("rtdbd supervisor started\n");
    fflush(stdout);

    for (;;) {
        if (g_stop) break;

        pid_t child = fork();
        if (child < 0) {
            usleep(10000);
            continue;
        }
        if (child == 0) {
            execv("/proc/self/exe", wargv);
            _exit(127);
        }

        write_pidfile(pidfile, child);
        printf("rtdbd worker pid=%d\n", (int)child);
        fflush(stdout);

        int status = 0;
        pid_t r = waitpid(child, &status, 0);
        if (r < 0 && errno != EINTR) break;

        if (g_stop) {
            kill(child, SIGTERM);
            break;
        }

        printf("rtdbd: worker %d exited, respawning\n", (int)child);
        fflush(stdout);
        usleep(1000); /* 极短退避，优先保证快速恢复 */
    }

    if (pidfile) unlink(pidfile);
    printf("rtdbd supervisor stopped\n");
    fflush(stdout);
    return 0;
}

int main(int argc, char** argv)
{
    const char* sock_path  = "/run/indurtdb/default.sock";
    const char* instance   = "default";
    const char* policy_path = NULL;
    const char* config_path = NULL;
    uint32_t    max_points = 10000;
    uint32_t    max_subs   = 32;
    bool        supervise  = false;
    const char* pidfile    = NULL;

    static struct option long_opts[] = {
        {"socket",     required_argument, 0, 's'},
        {"instance",   required_argument, 0, 'i'},
        {"policy",     required_argument, 0, 'p'},
        {"config",     required_argument, 0, 'c'},
        {"max-points", required_argument, 0, 'm'},
        {"max-subs",   required_argument, 0, 'b'},
        {"supervise",  no_argument,       0, 'S'},
        {"pidfile",    required_argument, 0, 'P'},
        {0, 0, 0, 0}
    };

    int c;
    while ((c = getopt_long(argc, argv, "s:i:p:c:m:b:SP:", long_opts, NULL)) != -1) {
        switch (c) {
        case 's': sock_path = optarg; break;
        case 'i': instance = optarg; break;
        case 'p': policy_path = optarg; break;
        case 'c': config_path = optarg; break;
        case 'm': max_points = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'b': max_subs = (uint32_t)strtoul(optarg, NULL, 10); break;
        case 'S': supervise = true; break;
        case 'P': pidfile = optarg; break;
        default: break;
        }
    }

    g_max_points = max_points; /* 供监控 LIST 遍历点位表使用 */

    if (supervise) {
        return supervise_loop(argc, argv, pidfile);
    }

    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    irt_policy_t policy;
    irt_policy_init(&policy);
    if (policy_path && irt_policy_load(&policy, policy_path) != 0) {
        fprintf(stderr, "rtdbd: failed to load policy: %s\n", policy_path);
        return 1;
    }

    irt_audit_t audit;
    irt_audit_init(&audit);

    if (indurtdb_initialize(instance, max_points, max_subs) != 0) {
        fprintf(stderr, "rtdbd: indurtdb_initialize failed: %s\n", indurtdb_get_last_error());
        return 1;
    }

    /* 注册点位名 / 元数据区初始化（写入共享索引，供 FIND_BY_NAME 端到端可用）。
     * 失败不致命：仅告警，守护进程继续提供写/读能力。 */
    if (config_path) {
        if (indurtdb_load_config(config_path) != 0) {
            fprintf(stderr, "rtdbd: load config failed: %s\n", indurtdb_get_last_error());
        }
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
        return 1;
    }

    printf("rtdbd listening on %s (instance=%s)\n", sock_path, instance);
    fflush(stdout);

    rtdbd_conn_t conns[RTDBD_MAX_CLIENTS];
    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) conn_init(&conns[i]);

    while (!g_stop) {
        struct pollfd fds[RTDBD_MAX_CLIENTS + 1];
        fds[0].fd = listen_fd;
        fds[0].events = POLLIN;
        int nfds = 1;

        for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
            if (conns[i].fd >= 0) {
                fds[nfds].fd = conns[i].fd;
                fds[nfds].events = POLLIN;
                nfds++;
            }
        }

        int ready = poll(fds, (nfds_t)nfds, 500);
        if (ready < 0) {
            if (errno == EINTR) continue;
            break;
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
            if (!(fds[i].revents & POLLIN)) continue;
            rtdbd_conn_t* conn = NULL;
            for (int k = 0; k < RTDBD_MAX_CLIENTS; ++k) {
                if (conns[k].fd == fds[i].fd) { conn = &conns[k]; break; }
            }
            if (!conn) continue;

            if (handle_request(fds[i].fd, &policy, &audit,
                               conn, conns, RTDBD_MAX_CLIENTS) != 0) {
                close(fds[i].fd);
                conn_init(conn);
            }
        }
    }

    for (int i = 0; i < RTDBD_MAX_CLIENTS; ++i) {
        if (conns[i].fd >= 0) close(conns[i].fd);
    }
    close(listen_fd);
    unlink(sock_path);

    indurtdb_shutdown();
    (void)now_ns();
    (void)RTDBD_SHUTDOWN_DELAY_SEC;
    return 0;
}
