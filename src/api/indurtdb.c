/**
 * @file indurtdb.c
 * @brief InduRTDB 主 API
 *   v3.4: 去单例化 (API v2 句柄化)。内部以 indurtdb_t 实例承载 shm/pm/sub,
 *         所有真实逻辑在 indurtdb_h_* 上; v1 全局函数 = 默认句柄 g_default 的薄封装。
 * @version 3.4.0
 * @date 2026-10-03
 * @copyright MIT License
 */

#include <indurtdb/indurtdb.h>
#include "core/irt_shm.h"
#include "core/irt_point_manager.h"
#include "core/irt_subscription.h"
#include "core/irt_config.h"
#include "core/irt_index.h"
#include <internal/irt_seqlock.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- 实例 (不透明句柄的内部定义) ---- */
struct indurtdb {
    irt_shm_t shm;
    irt_pm_t  pm;
    irt_sub_t sub;
    bool      initialized;
    int32_t   owner_pid;   /* fork 检测: 非零时 compare getpid() */
};

/* 定长实例表, 零堆分配 (满足"核心层无堆"不变式) */
#define IRT_MAX_INSTANCES 64
static indurtdb_t g_registry[IRT_MAX_INSTANCES];
static uint32_t   g_used_mask;          /* 槽位占用位图 */
static indurtdb_t* g_default = NULL;    /* v1 单例对应的实例句柄 */

/* 每个线程独立的错误信息, 无需锁保护 */
static _Thread_local char g_last_error[256];

static void set_error(const char* msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

/* ---- 实例槽位管理 ---- */
static indurtdb_t* inst_alloc(void) {
    for (uint32_t i = 0; i < IRT_MAX_INSTANCES; ++i) {
        if (!(g_used_mask & (1u << i))) {
            g_used_mask |= (1u << i);
            indurtdb_t* h = &g_registry[i];
            memset(h, 0, sizeof(*h));
            return h;
        }
    }
    return NULL;
}
static void inst_free(indurtdb_t* h);     /* 前向声明, 定义见下 */
static void inst_shutdown(indurtdb_t* h); /* 前向声明, 供 inst_init 使用 */

static void inst_free(indurtdb_t* h) {
    if (!h) return;
    for (uint32_t i = 0; i < IRT_MAX_INSTANCES; ++i) {
        if (h == &g_registry[i]) { g_used_mask &= ~(1u << i); return; }
    }
}

/* ---- 实例生命周期 (v2 真实实现) ---- */
static int inst_init(indurtdb_t* h, const char* instance_id,
                     uint32_t max_points, uint32_t max_subscribers) {
    /* fork 后子进程自动重置 (继承的 owner_pid 仍为父 PID, getpid() 已是子 PID) */
    if (h->initialized && h->owner_pid != 0
        && h->owner_pid != (int32_t)getpid()) {
        h->shm.os.owner = false;  /* 子进程不是 owner, 禁止 unlink 误删父段 */
        inst_shutdown(h);
    }
    if (h->initialized) { set_error("already initialized"); return INDURTDB_ERR_ARG; }
    if (!instance_id || instance_id[0] == '\0'
        || max_points == 0) { set_error("invalid argument"); return INDURTDB_ERR_ARG; }

    memset(h, 0, sizeof(*h));

    if (irt_shm_init(&h->shm, instance_id, max_points, max_subscribers) != 0) {
        set_error("shm init failed");
        return INDURTDB_ERR_ARG;
    }
    irt_pm_init(&h->pm, &h->shm);
    irt_sub_init(&h->sub, &h->shm);
    __atomic_store_n(&h->initialized, true, __ATOMIC_RELEASE);
    h->owner_pid = (int32_t)getpid();
    return 0;
}
static void inst_shutdown(indurtdb_t* h) {
    if (!__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) return;
    /* 先标记未初始化 (RELEASE), 阻止并发 ENSURE 通过; 之后 ACQUIRE 读到 false 即不再访问 shm */
    __atomic_store_n(&h->initialized, false, __ATOMIC_RELEASE);
    irt_shm_shutdown(&h->shm);
    memset(h, 0, sizeof(*h));
}

/* 写入成功后通知订阅者 (用栈变量读, 避免 _Thread_local 缓冲被回调重入覆盖) */
static int h_write_and_notify(indurtdb_t* h, int rc, uint32_t id) {
    if (rc == 0) {
        indurtdb_point_t pt;
        if (irt_pm_read(&h->pm, id, &pt) == 0)
            irt_sub_notify(&h->sub, id, &pt);
    }
    return rc;
}

#define ENSURE_H(h) do { \
    if (!h || !__atomic_load_n(&(h)->initialized, __ATOMIC_ACQUIRE)) \
        { set_error("not initialized"); return INDURTDB_ERR_NOT_INIT; } \
} while (0)

/* v1 兼容: v2 未初始化返回 INDURTDB_ERR_NOT_INIT, 但 v1 历史契约是 -1。
 * v1 薄封装统一把该码映射回 -1, 保证既有调用方(只判 <0)行为不变。
 * 必须是函数(非宏): 避免参数被求值两次导致写/读/计数被重复执行。 */
static inline int v1_int(int rc) {
    return rc == INDURTDB_ERR_NOT_INIT ? -1 : rc;
}

/* ============ API v2 (句柄化, 真实实现) ============ */

int indurtdb_h_open(indurtdb_t** out, const char* instance_id,
                    const indurtdb_cfg_t* cfg) {
    if (!out || !instance_id || instance_id[0] == '\0') {
        set_error("invalid argument"); return INDURTDB_ERR_ARG;
    }
    uint32_t mp = cfg ? cfg->max_points : 0;
    uint32_t ms = cfg ? cfg->max_subscribers : 0;
    if (mp == 0) { set_error("max_points must be > 0"); return INDURTDB_ERR_ARG; }

    indurtdb_t* h = inst_alloc();
    if (!h) { set_error("instance slots exhausted"); return INDURTDB_ERR_FULL; }

    int rc = inst_init(h, instance_id, mp, ms);
    if (rc != 0) { inst_free(h); return rc; }
    *out = h;
    return 0;
}

void indurtdb_h_close(indurtdb_t* h) {
    if (!h) return;
    inst_shutdown(h);
    if (h == g_default) g_default = NULL;
    inst_free(h);
}

bool indurtdb_h_is_initialized(const indurtdb_t* h) {
    return h && __atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE);
}

/* ---- 写入 ---- */
int indurtdb_h_write_bool(indurtdb_t* h, uint32_t id, bool value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_bool(&h->pm, id, value), id);
}
int indurtdb_h_write_int32(indurtdb_t* h, uint32_t id, int32_t value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_int32(&h->pm, id, value), id);
}
int indurtdb_h_write_double(indurtdb_t* h, uint32_t id, double value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_double(&h->pm, id, value), id);
}
int indurtdb_h_write_string(indurtdb_t* h, uint32_t id, const char* value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_string(&h->pm, id, value), id);
}
int indurtdb_h_write_bool_ts(indurtdb_t* h, uint32_t id, bool value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_bool_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_int32_ts(indurtdb_t* h, uint32_t id, int32_t value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_int32_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_double_ts(indurtdb_t* h, uint32_t id, double value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_double_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_string_ts(indurtdb_t* h, uint32_t id, const char* value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_string_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_int64(indurtdb_t* h, uint32_t id, int64_t value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_int64(&h->pm, id, value), id);
}
int indurtdb_h_write_uint32(indurtdb_t* h, uint32_t id, uint32_t value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_uint32(&h->pm, id, value), id);
}
int indurtdb_h_write_float(indurtdb_t* h, uint32_t id, float value) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_float(&h->pm, id, value), id);
}
int indurtdb_h_write_int64_ts(indurtdb_t* h, uint32_t id, int64_t value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_int64_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_uint32_ts(indurtdb_t* h, uint32_t id, uint32_t value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_uint32_ts(&h->pm, id, value, ts), id);
}
int indurtdb_h_write_float_ts(indurtdb_t* h, uint32_t id, float value, uint64_t ts) {
    ENSURE_H(h);
    return h_write_and_notify(h, irt_pm_write_float_ts(&h->pm, id, value, ts), id);
}

/* ---- 读取 ---- */
int indurtdb_h_read_bool(indurtdb_t* h, uint32_t id, bool* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.b;
    return 0;
}
int indurtdb_h_read_int32(indurtdb_t* h, uint32_t id, int32_t* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.i;
    return 0;
}
int indurtdb_h_read_double(indurtdb_t* h, uint32_t id, double* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.d;
    return 0;
}
int indurtdb_h_read_string(indurtdb_t* h, uint32_t id, char* buffer, size_t buffer_size) {
    ENSURE_H(h);
    if (!buffer || buffer_size == 0) { set_error("null or zero-size buffer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    snprintf(buffer, buffer_size, "%s", pt.value.str);
    return 0;
}
int indurtdb_h_read_int64(indurtdb_t* h, uint32_t id, int64_t* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.i64;
    return 0;
}
int indurtdb_h_read_uint32(indurtdb_t* h, uint32_t id, uint32_t* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.u32;
    return 0;
}
int indurtdb_h_read_float(indurtdb_t* h, uint32_t id, float* value) {
    ENSURE_H(h);
    if (!value) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    indurtdb_point_t pt;
    if (irt_pm_read(&h->pm, id, &pt) != 0) { set_error("read failed"); return INDURTDB_ERR_ARG; }
    *value = pt.value.f;
    return 0;
}
int indurtdb_h_read_point(indurtdb_t* h, uint32_t id, indurtdb_point_t* out) {
    ENSURE_H(h);
    if (!out) { set_error("null output pointer"); return INDURTDB_ERR_ARG; }
    return irt_pm_read(&h->pm, id, out);
}
const indurtdb_point_t* indurtdb_h_peek(indurtdb_t* h, uint32_t id) {
    if (!h || !__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) {
        set_error("not initialized"); return NULL;
    }
    return irt_pm_peek(&h->pm, id);
}
int indurtdb_h_read_range(indurtdb_t* h, uint32_t start_id, uint16_t count,
                          indurtdb_point_t* out_buf, uint16_t out_cap) {
    ENSURE_H(h);
    if (!out_buf || count == 0) { set_error("invalid argument"); return INDURTDB_ERR_ARG; }
    uint16_t n = (count < out_cap) ? count : out_cap;
    for (uint16_t i = 0; i < n; i++) {
        if (irt_pm_read(&h->pm, start_id + i, &out_buf[i]) != 0) {
            set_error("read failed"); return INDURTDB_ERR_ARG;
        }
    }
    return (int)n;
}
int indurtdb_h_write_range_bool(indurtdb_t* h, uint32_t start_id, const bool* values, uint16_t count) {
    ENSURE_H(h);
    if (!values) { set_error("null values pointer"); return INDURTDB_ERR_ARG; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = h_write_and_notify(h, irt_pm_write_bool(&h->pm, start_id + i, values[i]), start_id + i);
        if (rc != 0) return (int)i;
    }
    return (int)count;
}
int indurtdb_h_write_range_int32(indurtdb_t* h, uint32_t start_id, const int32_t* values, uint16_t count) {
    ENSURE_H(h);
    if (!values) { set_error("null values pointer"); return INDURTDB_ERR_ARG; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = h_write_and_notify(h, irt_pm_write_int32(&h->pm, start_id + i, values[i]), start_id + i);
        if (rc != 0) return (int)i;
    }
    return (int)count;
}
int indurtdb_h_write_range_double(indurtdb_t* h, uint32_t start_id, const double* values, uint16_t count) {
    ENSURE_H(h);
    if (!values) { set_error("null values pointer"); return INDURTDB_ERR_ARG; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = h_write_and_notify(h, irt_pm_write_double(&h->pm, start_id + i, values[i]), start_id + i);
        if (rc != 0) return (int)i;
    }
    return (int)count;
}

/* ---- 订阅 ---- */
int indurtdb_h_subscribe(indurtdb_t* h, uint32_t id, indurtdb_callback_t cb, void* user_data) {
    ENSURE_H(h);
    return irt_sub_subscribe(&h->sub, id, cb, user_data);
}
int indurtdb_h_unsubscribe(indurtdb_t* h, uint32_t id) {
    ENSURE_H(h);
    return irt_sub_unsubscribe(&h->sub, id);
}

/* ---- 配置/心跳 ---- */
int indurtdb_h_load_config(indurtdb_t* h, const char* config_path) {
    ENSURE_H(h);
    if (!config_path) { set_error("null config path"); return INDURTDB_ERR_ARG; }

    irt_point_meta_batch_t batch;
    memset(&batch, 0, sizeof(batch));
    int n = irt_point_config_parse_yaml(config_path, &batch);
    if (n < 0) { set_error("config parse failed"); return INDURTDB_ERR_ARG; }

    indurtdb_point_t* pts = irt_shm_points(&h->shm);
    uint32_t maxp = h->shm.max_points;
    if (!pts || maxp == 0) { irt_point_config_free(&batch); return INDURTDB_ERR_ARG; }
    irt_header_t* hdr = irt_shm_header(&h->shm);
    if (!hdr) { irt_point_config_free(&batch); return INDURTDB_ERR_ARG; }
    for (int i = 0; i < n; ++i) {
        const irt_point_meta_t* pm = &batch.points[i];
        if (pm->id >= maxp) continue;
        if (pm->type > INDURTDB_TYPE_STRING) continue;

        uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
        if (seq0 & 1ULL) continue;

        indurtdb_point_t* p = &pts[pm->id];
        p->type   = pm->type;
        p->unit   = pm->unit;
        p->access = pm->access;
        strncpy(p->name, pm->name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';

        __atomic_thread_fence(__ATOMIC_RELEASE);
        irt_seqlock_write_end(&hdr->write_seq, seq0);

        /* 注册进本实例的 name→id 索引 (写锁释放后单独取锁, 避免嵌套自锁) */
        irt_index_insert(&h->shm, p->name, pm->id);
    }
    irt_point_config_free(&batch);
    return 0;
}

int indurtdb_h_set_quality(indurtdb_t* h, uint32_t id, uint8_t quality) {
    ENSURE_H(h);
    return irt_pm_set_quality(&h->pm, id, quality);
}
void indurtdb_h_update_heartbeat(indurtdb_t* h) {
    if (!h || !__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) return;
    irt_sub_update_heartbeat(&h->sub, (int32_t)getpid());
}

/* ---- 索引 / 元数据 ---- */
int indurtdb_h_find_by_name(indurtdb_t* h, const char* name, uint32_t* out_id) {
    ENSURE_H(h);
    if (!name || name[0] == '\0' || !out_id) {
        set_error("invalid argument"); return INDURTDB_ERR_ARG;
    }
    int rc = irt_index_lookup(&h->shm, name, out_id);
    if (rc == INDURTDB_ERR_NOT_FOUND) set_error("point name not found");
    else if (rc == INDURTDB_ERR_BUSY) set_error("index busy, retry");
    else if (rc != INDURTDB_OK)       set_error("index lookup failed");
    return rc;
}
int indurtdb_h_get_meta(indurtdb_t* h, uint32_t id, indurtdb_meta_t* meta) {
    ENSURE_H(h);
    if (!meta) { set_error("null meta"); return INDURTDB_ERR_ARG; }
    int rc = irt_meta_get(&h->shm, id, meta);
    if (rc == INDURTDB_ERR_ARG)       set_error("id out of range");
    else if (rc == INDURTDB_ERR_BUSY) set_error("meta busy, retry");
    else if (rc != INDURTDB_OK)       set_error("meta get failed");
    return rc;
}
int indurtdb_h_set_meta(indurtdb_t* h, uint32_t id, const indurtdb_meta_t* meta) {
    ENSURE_H(h);
    if (!meta) { set_error("null meta"); return INDURTDB_ERR_ARG; }
    int rc = irt_meta_set(&h->shm, id, meta);
    if (rc == INDURTDB_ERR_ARG)       set_error("id out of range");
    else if (rc == INDURTDB_ERR_BUSY) set_error("meta busy, retry");
    else if (rc != INDURTDB_OK)       set_error("meta set failed");
    return rc;
}

/* ---- 校验/统计 ---- */
int indurtdb_h_check_timeouts(indurtdb_t* h, uint64_t timeout_ns) {
    ENSURE_H(h);
    return irt_pm_check_timeouts(&h->pm, timeout_ns);
}
uint64_t indurtdb_h_get_write_count(indurtdb_t* h) {
    if (!h || !__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) return 0;
    return irt_pm_write_count(&h->pm);
}
uint64_t indurtdb_h_get_timeout_count(indurtdb_t* h) {
    if (!h || !__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) return 0;
    irt_header_t* hdr = irt_shm_header(&h->shm);
    return hdr ? __atomic_load_n(&hdr->stats.timeouts, __ATOMIC_RELAXED) : 0;
}
int indurtdb_h_validate_id(indurtdb_t* h, uint32_t id) {
    if (!h || !__atomic_load_n(&h->initialized, __ATOMIC_ACQUIRE)) return 0;
    return irt_pm_validate_id(&h->pm, id) ? 1 : 0;
}

/* ============ API v1 (默认句柄 g_default 的薄封装, 行为不变) ============ */

int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points, uint32_t max_subscribers) {
    /* fork 后子进程自动重置 (沿用 v3.1 的 owner 检测逻辑) */
    if (g_default && __atomic_load_n(&g_default->initialized, __ATOMIC_ACQUIRE)) {
        if (g_default->owner_pid != 0
            && g_default->owner_pid != (int32_t)getpid()) {
            g_default->shm.os.owner = false;
            inst_shutdown(g_default);
        } else {
            set_error("already initialized");
            return INDURTDB_ERR_ARG;
        }
    }
    if (!g_default) {
        g_default = inst_alloc();
        if (!g_default) { set_error("instance slots exhausted"); return INDURTDB_ERR_FULL; }
    }
    int rc = inst_init(g_default, instance_id, max_points, max_subscribers);
    if (rc != 0) { inst_free(g_default); g_default = NULL; }
    return rc;
}

void indurtdb_shutdown(void) {
    if (!g_default) return;
    indurtdb_h_close(g_default);   /* 内部会复位 g_default */
}

bool indurtdb_is_initialized(void) {
    return indurtdb_h_is_initialized(g_default);
}

int indurtdb_write_bool(uint32_t id, bool value) {
    return v1_int(indurtdb_h_write_bool(g_default, id, value));
}
int indurtdb_write_int32(uint32_t id, int32_t value) {
    return v1_int(indurtdb_h_write_int32(g_default, id, value));
}
int indurtdb_write_double(uint32_t id, double value) {
    return v1_int(indurtdb_h_write_double(g_default, id, value));
}
int indurtdb_write_string(uint32_t id, const char* value) {
    return v1_int(indurtdb_h_write_string(g_default, id, value));
}
int indurtdb_write_int64(uint32_t id, int64_t value) {
    return v1_int(indurtdb_h_write_int64(g_default, id, value));
}
int indurtdb_write_uint32(uint32_t id, uint32_t value) {
    return v1_int(indurtdb_h_write_uint32(g_default, id, value));
}
int indurtdb_write_float(uint32_t id, float value) {
    return v1_int(indurtdb_h_write_float(g_default, id, value));
}
int indurtdb_write_bool_ts(uint32_t id, bool value, uint64_t ts) {
    return v1_int(indurtdb_h_write_bool_ts(g_default, id, value, ts));
}
int indurtdb_write_int32_ts(uint32_t id, int32_t value, uint64_t ts) {
    return v1_int(indurtdb_h_write_int32_ts(g_default, id, value, ts));
}
int indurtdb_write_double_ts(uint32_t id, double value, uint64_t ts) {
    return v1_int(indurtdb_h_write_double_ts(g_default, id, value, ts));
}
int indurtdb_write_string_ts(uint32_t id, const char* value, uint64_t ts) {
    return v1_int(indurtdb_h_write_string_ts(g_default, id, value, ts));
}
int indurtdb_write_int64_ts(uint32_t id, int64_t value, uint64_t ts) {
    return v1_int(indurtdb_h_write_int64_ts(g_default, id, value, ts));
}
int indurtdb_write_uint32_ts(uint32_t id, uint32_t value, uint64_t ts) {
    return v1_int(indurtdb_h_write_uint32_ts(g_default, id, value, ts));
}
int indurtdb_write_float_ts(uint32_t id, float value, uint64_t ts) {
    return v1_int(indurtdb_h_write_float_ts(g_default, id, value, ts));
}
int indurtdb_read_bool(uint32_t id, bool* value) {
    return v1_int(indurtdb_h_read_bool(g_default, id, value));
}
int indurtdb_read_int32(uint32_t id, int32_t* value) {
    return v1_int(indurtdb_h_read_int32(g_default, id, value));
}
int indurtdb_read_double(uint32_t id, double* value) {
    return v1_int(indurtdb_h_read_double(g_default, id, value));
}
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size) {
    return v1_int(indurtdb_h_read_string(g_default, id, buffer, buffer_size));
}
int indurtdb_read_int64(uint32_t id, int64_t* value) {
    return v1_int(indurtdb_h_read_int64(g_default, id, value));
}
int indurtdb_read_uint32(uint32_t id, uint32_t* value) {
    return v1_int(indurtdb_h_read_uint32(g_default, id, value));
}
int indurtdb_read_float(uint32_t id, float* value) {
    return v1_int(indurtdb_h_read_float(g_default, id, value));
}
int indurtdb_read_point(uint32_t id, indurtdb_point_t* out) {
    return v1_int(indurtdb_h_read_point(g_default, id, out));
}
const indurtdb_point_t* indurtdb_peek(uint32_t id) {
    return indurtdb_h_peek(g_default, id);   /* 未初始化返回 NULL, 与 v1 一致 */
}
int indurtdb_read_range(uint32_t start_id, uint16_t count,
                        indurtdb_point_t* out_buf, uint16_t out_cap) {
    return v1_int(indurtdb_h_read_range(g_default, start_id, count, out_buf, out_cap));
}
int indurtdb_write_range_bool(uint32_t start_id, const bool* values, uint16_t count) {
    return v1_int(indurtdb_h_write_range_bool(g_default, start_id, values, count));
}
int indurtdb_write_range_int32(uint32_t start_id, const int32_t* values, uint16_t count) {
    return v1_int(indurtdb_h_write_range_int32(g_default, start_id, values, count));
}
int indurtdb_write_range_double(uint32_t start_id, const double* values, uint16_t count) {
    return v1_int(indurtdb_h_write_range_double(g_default, start_id, values, count));
}
int indurtdb_subscribe(uint32_t id, indurtdb_callback_t cb, void* user_data) {
    return v1_int(indurtdb_h_subscribe(g_default, id, cb, user_data));
}
int indurtdb_unsubscribe(uint32_t id) {
    return v1_int(indurtdb_h_unsubscribe(g_default, id));
}
int indurtdb_load_config(const char* config_path) {
    return v1_int(indurtdb_h_load_config(g_default, config_path));
}
int indurtdb_set_quality(uint32_t id, uint8_t quality) {
    return v1_int(indurtdb_h_set_quality(g_default, id, quality));
}
void indurtdb_update_heartbeat(void) {
    indurtdb_h_update_heartbeat(g_default);
}
int indurtdb_find_by_name(const char* name, uint32_t* out_id) {
    return v1_int(indurtdb_h_find_by_name(g_default, name, out_id));
}
int indurtdb_get_meta(uint32_t id, indurtdb_meta_t* meta) {
    return v1_int(indurtdb_h_get_meta(g_default, id, meta));
}
int indurtdb_set_meta(uint32_t id, const indurtdb_meta_t* meta) {
    return v1_int(indurtdb_h_set_meta(g_default, id, meta));
}
int indurtdb_check_timeouts(uint64_t timeout_ns) {
    return v1_int(indurtdb_h_check_timeouts(g_default, timeout_ns));
}
uint64_t indurtdb_get_write_count(void) {
    return indurtdb_h_get_write_count(g_default);   /* 未初始化返回 0, 与 v1 一致 */
}
uint64_t indurtdb_get_timeout_count(void) {
    return indurtdb_h_get_timeout_count(g_default);
}
int indurtdb_validate_id(uint32_t id) {
    return v1_int(indurtdb_h_validate_id(g_default, id));
}
const char* indurtdb_get_last_error(void) {
    return g_last_error;
}

/* ==== v3.4 T6: 质量语义与 OPC UA 映射（纯函数，无状态） ====
 * 映射表参考 OPC UA Part 4 的 StatusCode；base 码决定 severity + 子状态，
 * limit 位映射到 StatusCode 保留位(bit28-29)——远离 severity(bit30-31)，且避开
 * OPC UA 已定义位(bit24 StructureChanged / bit25 SemanticsChanged)，不污染 severity 与
 * code(bit0-15)。未知 StatusCode 回退为 BAD。映射表可按现场需求调整。 */
static const struct {
    uint8_t  base;
    uint32_t sc;
} k_quality_to_sc[] = {
    { INDURTDB_QUALITY_GOOD,           0x00000000u }, /* Good */
    { INDURTDB_QUALITY_BAD,            0x80000000u }, /* Bad */
    { INDURTDB_QUALITY_TIMEOUT,        0x408F0000u }, /* Uncertain_SensorNotAccurate */
    { INDURTDB_QUALITY_SUBSTITUTED,    0x00D90000u }, /* Good_LocalOverride */
    { INDURTDB_QUALITY_UNCERTAIN,      0x40000000u }, /* Uncertain */
    { INDURTDB_QUALITY_NOT_INITIALIZED,0x408D0000u }, /* Uncertain_InitialValue */
    { INDURTDB_QUALITY_OUT_OF_SERVICE, 0x808D0000u }, /* Bad_OutOfService */
    { INDURTDB_QUALITY_COMM_FAILURE,   0x80870000u }, /* Bad_NoCommunication */
    { INDURTDB_QUALITY_SENSOR_FAILURE, 0x808A0000u }, /* Bad_SensorFailure */
    { INDURTDB_QUALITY_LAST_KNOWN,     0x40900000u }, /* Uncertain_LastUsableValue */
    { INDURTDB_QUALITY_CONFIG_ERROR,   0x80A10000u }, /* Bad_ConfigurationError */
};

uint32_t indurtdb_quality_to_status_code(uint8_t quality) {
    uint8_t  base  = INDURTDB_QUALITY_BASE(quality);
    uint8_t  limit = INDURTDB_QUALITY_LIMIT(quality);
    uint32_t sc    = 0x80000000u; /* 默认 Bad */
    for (size_t i = 0; i < sizeof(k_quality_to_sc) / sizeof(k_quality_to_sc[0]); i++) {
        if (k_quality_to_sc[i].base == base) { sc = k_quality_to_sc[i].sc; break; }
    }
    /* 量程位放进 StatusCode 保留位(bit28-29)，与 severity(bit30-31)/code(bit0-15) 正交 */
    sc |= ((uint32_t)limit & 0x3u) << 28;
    return sc;
}

uint8_t indurtdb_status_code_to_quality(uint32_t status_code) {
    uint8_t  limit = (uint8_t)((status_code >> 28) & 0x3u);
    uint32_t raw   = status_code & ~((uint32_t)0x3u << 28);  /* 抹去我们的 limit 位(bit28-29) */
    uint8_t  base  = INDURTDB_QUALITY_BAD;                    /* 默认 Bad */
    for (size_t i = 0; i < sizeof(k_quality_to_sc) / sizeof(k_quality_to_sc[0]); i++) {
        if (k_quality_to_sc[i].sc == raw) { base = k_quality_to_sc[i].base; break; }
    }
    return INDURTDB_QUALITY_MAKE(base, limit);
}

bool indurtdb_quality_is_usable(uint8_t quality) {
    return INDURTDB_QUALITY_BASE(quality) == INDURTDB_QUALITY_GOOD;
}
