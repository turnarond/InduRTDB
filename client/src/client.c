/**
 * @file client.c
 * @brief indurtdb-client —— rtdbd 的 RPC 客户端（含本地写队列）
 *
 * 与核心库 libindurtdb.a 并列但独立：核心库不感知 RPC，本客户端负责
 * 与 rtdbd 通信，并在服务不可用时把写缓存在本地队列（fail-operational）。
 */
#include <irtcli/client.h>

#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <rtdbd/protocol.h>

/* ---- 内部：队列操作（定长环形，init 时一次分配） ---- */

/* 同点位合并去重：已存在则覆盖值（保留首次入队位置），否则追加。
 * 返回 IRTCLI_OK / IRTCLI_ERR_FULL */
static int q_push(irtcli_t* c, const irtcli_entry_t* e)
{
    uint32_t n = c->count;
    uint32_t start = (c->head + c->cap - n) % c->cap;

    for (uint32_t i = 0; i < n; ++i) {
        irtcli_entry_t* slot = &c->queue[(start + i) % c->cap];
        if (slot->point_id == e->point_id) {
            slot->type         = e->type;
            slot->value_bits   = e->value_bits;
            slot->source_ts_ns = e->source_ts_ns;
            return IRTCLI_OK;
        }
    }

    if (n >= c->cap) {
        if (c->alert) c->alert("irtcli: write queue full", c->user_data);
        return IRTCLI_ERR_FULL;
    }

    c->queue[c->head] = *e;
    c->head = (c->head + 1u) % c->cap;
    c->count++;
    return IRTCLI_OK;
}

/* 弹出最旧的一条。成功返回 true */
static bool q_pop(irtcli_t* c, irtcli_entry_t* out)
{
    if (c->count == 0) return false;

    uint32_t start = (c->head + c->cap - c->count) % c->cap;
    *out = c->queue[start];
    c->count--;
    return true;
}

/* ---- 内部：IO ---- */

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
        if (n == 0) return -1;
        p += n;
        len -= (size_t)n;
    }
    return 0;
}

/* 提交单条写入。返回 IRTCLI_OK / 负错误码 */
static int submit_one(irtcli_t* c, const irtcli_entry_t* e)
{
    rtdbd_req_hdr_t  req;
    rtdbd_write_req_t w;

    memset(&req, 0, sizeof(req));
    memset(&w, 0, sizeof(w));

    req.magic       = RTDBD_MAGIC;
    req.version     = RTDBD_PROTO_VERSION;
    req.opcode      = RTDBD_OP_WRITE;
    req.payload_len = (uint32_t)sizeof(w);

    w.point_id      = e->point_id;
    w.type          = e->type;
    w.value_bits    = e->value_bits;
    w.source_ts_ns  = e->source_ts_ns;

    if (send_all(c->fd, &req, sizeof(req)) != 0) return IRTCLI_ERR_IO;
    if (send_all(c->fd, &w, sizeof(w)) != 0) return IRTCLI_ERR_IO;

    rtdbd_resp_hdr_t resp;
    if (recv_all(c->fd, &resp, sizeof(resp)) != 0) return IRTCLI_ERR_IO;

    if (resp.magic != RTDBD_MAGIC || resp.version != RTDBD_PROTO_VERSION) {
        return IRTCLI_ERR_PROTO;
    }
    switch (resp.status) {
    case RTDBD_ST_OK:     return IRTCLI_OK;
    case RTDBD_ST_DENIED: return IRTCLI_ERR_DENIED;
    default:              return IRTCLI_ERR_PROTO;
    }
}

/* ---- 公开 API ---- */

int irtcli_init(irtcli_t* c, const char* sock_path, uint32_t cap,
                irtcli_alert_fn alert, void* user_data)
{
    if (!c || !sock_path) return IRTCLI_ERR_ARG;

    uint32_t real_cap = (cap == 0) ? IRTCLI_QUEUE_CAP_DEFAULT : cap;
    if (real_cap > IRTCLI_QUEUE_CAP_MAX) return IRTCLI_ERR_ARG;

    size_t plen = strlen(sock_path);
    if (plen >= IRTCLI_SOCK_PATH_MAX) return IRTCLI_ERR_ARG;

    memset(c, 0, sizeof(*c));
    memcpy(c->sock_path, sock_path, plen + 1);
    c->cap       = real_cap;
    c->async     = true;
    c->alert     = alert;
    c->user_data = user_data;
    c->fd        = -1;
    c->connected = false;
    c->queue     = (irtcli_entry_t*)calloc(c->cap, sizeof(irtcli_entry_t));
    if (!c->queue) return IRTCLI_ERR_IO;
    return IRTCLI_OK;
}

void irtcli_close(irtcli_t* c)
{
    if (!c) return;
    if (c->fd >= 0) close(c->fd);
    free(c->queue);
    memset(c, 0, sizeof(*c));
}

int irtcli_connect(irtcli_t* c)
{
    if (!c) return IRTCLI_ERR_ARG;

    if (c->fd >= 0) {
        close(c->fd);
        c->fd = -1;
        c->connected = false;
    }

    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return IRTCLI_ERR_IO;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    size_t plen = strlen(c->sock_path);
    if (plen >= sizeof(addr.sun_path)) {
        close(fd);
        return IRTCLI_ERR_ARG;
    }
    memcpy(addr.sun_path, c->sock_path, plen + 1);

    if (connect(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0) {
        close(fd);
        return IRTCLI_ERR_IO;
    }

    c->fd = fd;
    c->connected = true;
    return IRTCLI_OK;
}

void irtcli_set_async(irtcli_t* c, bool async)
{
    if (c) c->async = async;
}

/* 异步入队 / 同步直提 */
static int write_entry(irtcli_t* c, const irtcli_entry_t* e)
{
    if (!c || !e) return IRTCLI_ERR_ARG;

    if (c->async) {
        int rc = q_push(c, e);
        /* 入队成功对外表达为 IRTCLI_QUEUED（区别于同步的 IRTCLI_OK 已提交） */
        return (rc == IRTCLI_OK) ? IRTCLI_QUEUED : rc;
    }

    if (!c->connected && irtcli_connect(c) != IRTCLI_OK) {
        return IRTCLI_ERR_IO;
    }
    int rc = submit_one(c, e);
    if (rc == IRTCLI_ERR_IO) {
        c->connected = false; /* 连接失效，下次重试 */
    }
    return rc;
}

int irtcli_write_bool(irtcli_t* c, uint32_t id, bool value, uint64_t source_ts_ns)
{
    irtcli_entry_t e;
    memset(&e, 0, sizeof(e));
    e.point_id      = id;
    e.type          = IRTCLI_TYPE_BOOL;
    e.value_bits    = value ? 1u : 0u;
    e.source_ts_ns  = source_ts_ns;
    return write_entry(c, &e);
}

int irtcli_write_int32(irtcli_t* c, uint32_t id, int32_t value, uint64_t source_ts_ns)
{
    irtcli_entry_t e;
    memset(&e, 0, sizeof(e));
    e.point_id      = id;
    e.type          = IRTCLI_TYPE_INT32;
    e.value_bits    = (uint64_t)(int64_t)value;
    e.source_ts_ns  = source_ts_ns;
    return write_entry(c, &e);
}

int irtcli_write_double(irtcli_t* c, uint32_t id, double value, uint64_t source_ts_ns)
{
    irtcli_entry_t e;
    memset(&e, 0, sizeof(e));
    e.point_id     = id;
    e.type         = IRTCLI_TYPE_DOUBLE;
    memcpy(&e.value_bits, &value, sizeof(value));
    e.source_ts_ns = source_ts_ns;
    return write_entry(c, &e);
}

int irtcli_flush(irtcli_t* c)
{
    if (!c) return IRTCLI_ERR_ARG;

    if (c->count == 0) return 0;

    if (!c->connected && irtcli_connect(c) != IRTCLI_OK) {
        return 0; /* 连接不可用：保留队列，不丢数据 */
    }

    int submitted = 0;
    irtcli_entry_t e;
    while (q_pop(c, &e)) {
        int rc = submit_one(c, &e);
        if (rc != IRTCLI_OK) {
            /* 失败：恢复计数即可把该条放回队首（数据仍在槽位中，顺序不变） */
            if (rc == IRTCLI_ERR_IO) c->connected = false;
            if (c->alert) c->alert("irtcli: submit failed, entry retained", c->user_data);
            c->count++;
            break;
        }
        submitted++;
    }
    return submitted;
}

uint32_t irtcli_queue_count(const irtcli_t* c)
{
    return c ? c->count : 0u;
}
