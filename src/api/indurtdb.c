/**
 * @file indurtdb.c
 * @brief InduRTDB 主 API — 单例串联所有模块
 * @version 3.0.0
 * @date 2026-07-15
 * @copyright MIT License
 */

#include <indurtdb/indurtdb.h>
#include "core/irt_shm.h"
#include "core/irt_point_manager.h"
#include "core/irt_subscription.h"
#include "core/irt_config.h"
#include <internal/irt_seqlock.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- 全局单例 ---- */
static struct {
    irt_shm_t shm;
    irt_pm_t  pm;
    irt_sub_t sub;
    bool    initialized;
    int32_t   owner_pid;   /* fork 检测: 非零时 compare getpid() */
} g_rtdb;

/* 每个线程独立的错误信息，无需锁保护 */
static _Thread_local char g_last_error[256];

static void set_error(const char* msg) {
    snprintf(g_last_error, sizeof(g_last_error), "%s", msg);
}

/* ---- 生命周期 ---- */

int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points, uint32_t max_subscribers) {
    /* fork 后子进程自动重置: 继承的 owner_pid 仍为父进程 PID,
     * 但 getpid() 已返回子进程 PID —— 不匹配则自动重置.
     *
     * 关键: 子进程通过 fork 继承了父进程的 irt_shm_os_t.owner=true,
     * 直接 shutdown 会触发 shm_unlink 销毁父进程创建的共享内存名称,
     * 导致后续 re-init 时 shm_open(O_EXCL) 创建全新空段而非 attach.
     * 必须先清除 owner 标记, 确保 shutdown 仅做进程本地 detach. */
    if (g_rtdb.initialized && g_rtdb.owner_pid != 0
        && g_rtdb.owner_pid != (int32_t)getpid()) {
        g_rtdb.shm.os.owner = false;  /* 子进程不是 owner, 禁止 unlink */
        indurtdb_shutdown();
    }
    if (g_rtdb.initialized) { set_error("already initialized"); return -1; }
    if (!instance_id || instance_id[0] == '\0'
        || max_points == 0) { set_error("invalid argument"); return -1; }

    memset(&g_rtdb, 0, sizeof(g_rtdb));

    if (irt_shm_init(&g_rtdb.shm, instance_id, max_points,
                     max_subscribers) != 0) {
        set_error("shm init failed");
        return -1;
    }

    irt_pm_init(&g_rtdb.pm, &g_rtdb.shm);
    irt_sub_init(&g_rtdb.sub, &g_rtdb.shm);
    __atomic_store_n(&g_rtdb.initialized, true, __ATOMIC_RELEASE);
    g_rtdb.owner_pid = (int32_t)getpid();
    return 0;
}

void indurtdb_shutdown(void) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) return;
    /* 先标记为未初始化 (RELEASE 语义), 阻止并发 ENSURE_INIT 通过;
     * 在此之后的 ENSURE_INIT ACQUIRE 读到 false 后返回 -1, 不会再访问 shm. */
    __atomic_store_n(&g_rtdb.initialized, false, __ATOMIC_RELEASE);
    /* 然后释放共享内存资源 */
    irt_shm_shutdown(&g_rtdb.shm);
    memset(&g_rtdb, 0, sizeof(g_rtdb));
}

bool indurtdb_is_initialized(void) {
    return __atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE);
}

/* ---- 写入 ---- */

/* 写入成功后通知订阅者. irt_pm_write_* 返回 0=成功, 负值=失败.
 * 使用 irt_pm_read(栈变量) 而非 irt_pm_peek(_Thread_local 缓冲),
 * 因为通知回调可能重入 write_and_notify, 覆盖 _Thread_local 缓冲. */
static int write_and_notify(int rc, uint32_t id) {
    if (rc == 0) {
        indurtdb_point_t pt;
        if (irt_pm_read(&g_rtdb.pm, id, &pt) == 0)
            irt_sub_notify(&g_rtdb.sub, id, &pt);
    }
    return rc;
}

/* __atomic_load_n 确保多线程可见; initialized 由 shutdown 通过 __atomic_store_n(RELEASE) 清零 */
#define ENSURE_INIT() do { \
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) \
        { set_error("not initialized"); return -1; } \
} while (0)

int indurtdb_write_bool(uint32_t id, bool value) {
    ENSURE_INIT();
    return write_and_notify(irt_pm_write_bool(&g_rtdb.pm, id, value), id);
}
int indurtdb_write_int32(uint32_t id, int32_t value) {
    ENSURE_INIT();
    return write_and_notify(irt_pm_write_int32(&g_rtdb.pm, id, value), id);
}
int indurtdb_write_double(uint32_t id, double value) {
    ENSURE_INIT();
    return write_and_notify(irt_pm_write_double(&g_rtdb.pm, id, value), id);
}
int indurtdb_write_string(uint32_t id, const char* value) {
    ENSURE_INIT();
    return write_and_notify(irt_pm_write_string(&g_rtdb.pm, id, value), id);
}

/* ---- 读取 ---- */

int indurtdb_read_bool(uint32_t id, bool* value) {
    ENSURE_INIT();
    if (!value) { set_error("null output pointer"); return -1; }
    indurtdb_point_t pt;
    if (irt_pm_read(&g_rtdb.pm, id, &pt) != 0) { set_error("read failed"); return -1; }
    *value = pt.value.b;
    return 0;
}
int indurtdb_read_int32(uint32_t id, int32_t* value) {
    ENSURE_INIT();
    if (!value) { set_error("null output pointer"); return -1; }
    indurtdb_point_t pt;
    if (irt_pm_read(&g_rtdb.pm, id, &pt) != 0) { set_error("read failed"); return -1; }
    *value = pt.value.i;
    return 0;
}
int indurtdb_read_double(uint32_t id, double* value) {
    ENSURE_INIT();
    if (!value) { set_error("null output pointer"); return -1; }
    indurtdb_point_t pt;
    if (irt_pm_read(&g_rtdb.pm, id, &pt) != 0) { set_error("read failed"); return -1; }
    *value = pt.value.d;
    return 0;
}
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size) {
    ENSURE_INIT();
    if (!buffer || buffer_size == 0) { set_error("null or zero-size buffer"); return -1; }
    indurtdb_point_t pt;
    if (irt_pm_read(&g_rtdb.pm, id, &pt) != 0) { set_error("read failed"); return -1; }
    snprintf(buffer, buffer_size, "%s", pt.value.str);
    return 0;
}
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data) {
    ENSURE_INIT();
    if (!point_data) { set_error("null output pointer"); return -1; }
    return irt_pm_read(&g_rtdb.pm, id, point_data);
}
const indurtdb_point_t* indurtdb_peek(uint32_t id) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) { set_error("not initialized"); return NULL; }
    return irt_pm_peek(&g_rtdb.pm, id);
}

/* ---- 批量 ---- */

int indurtdb_read_range(uint32_t start_id, uint16_t count,
                        indurtdb_point_t* out_buf, uint16_t out_cap) {
    ENSURE_INIT();
    if (!out_buf || count == 0) { set_error("invalid argument"); return -1; }
    uint16_t n = (count < out_cap) ? count : out_cap;
    for (uint16_t i = 0; i < n; i++) {
        if (irt_pm_read(&g_rtdb.pm, start_id + i, &out_buf[i]) != 0)
            { set_error("read failed"); return -1; }
    }
    return n;
}
int indurtdb_write_range_bool(uint32_t start_id, const bool* values,
                              uint16_t count) {
    ENSURE_INIT();
    if (!values) { set_error("null values pointer"); return -1; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = write_and_notify(
            irt_pm_write_bool(&g_rtdb.pm, start_id + i, values[i]),
            start_id + i);
        if (rc != 0) return i;
    }
    return count;
}
int indurtdb_write_range_int32(uint32_t start_id, const int32_t* values,
                                uint16_t count) {
    ENSURE_INIT();
    if (!values) { set_error("null values pointer"); return -1; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = write_and_notify(
            irt_pm_write_int32(&g_rtdb.pm, start_id + i, values[i]),
            start_id + i);
        if (rc != 0) return i;
    }
    return count;
}
int indurtdb_write_range_double(uint32_t start_id, const double* values,
                                 uint16_t count) {
    ENSURE_INIT();
    if (!values) { set_error("null values pointer"); return -1; }
    for (uint16_t i = 0; i < count; i++) {
        int rc = write_and_notify(
            irt_pm_write_double(&g_rtdb.pm, start_id + i, values[i]),
            start_id + i);
        if (rc != 0) return i;
    }
    return count;
}

/* ---- 订阅 ---- */

int indurtdb_subscribe(uint32_t id, indurtdb_callback_t cb, void* user_data) {
    ENSURE_INIT();
    return irt_sub_subscribe(&g_rtdb.sub, id, cb, user_data);
}
int indurtdb_unsubscribe(uint32_t id) {
    ENSURE_INIT();
    return irt_sub_unsubscribe(&g_rtdb.sub, id);
}

/* ---- 配置/心跳 ---- */

int indurtdb_load_config(const char* config_path) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) {
        set_error("not initialized"); return -1;
    }
    if (!config_path) { set_error("null config path"); return -1; }

    irt_point_meta_batch_t batch;
    memset(&batch, 0, sizeof(batch));
    int n = irt_point_config_parse_yaml(config_path, &batch);
    if (n < 0) { set_error("config parse failed"); return -1; }

    /* 将解析出的元数据写入已存在的点位中 */
    indurtdb_point_t* pts = irt_shm_points(&g_rtdb.shm);
    uint32_t maxp = g_rtdb.shm.max_points;
    if (!pts || maxp == 0) { irt_point_config_free(&batch); return -1; }
    irt_header_t* hdr = irt_shm_header(&g_rtdb.shm);
    if (!hdr) { irt_point_config_free(&batch); return -1; }
    for (int i = 0; i < n; ++i) {
        const irt_point_meta_t* pm = &batch.points[i];
        if (pm->id >= maxp) continue;
        /* 跳过未知类型 (irt_config_parse_type 返回 0xff) */
        if (pm->type > INDURTDB_TYPE_STRING) continue;

        uint64_t seq0 = irt_seqlock_write_begin(&hdr->write_seq);
        if (seq0 & 1ULL) continue;  /* 写冲突,跳过此点 */

        indurtdb_point_t* p = &pts[pm->id];
        p->type   = pm->type;
        p->unit   = pm->unit;
        p->access = pm->access;
        strncpy(p->name, pm->name, sizeof(p->name) - 1);
        p->name[sizeof(p->name) - 1] = '\0';

        __atomic_thread_fence(__ATOMIC_RELEASE);
        irt_seqlock_write_end(&hdr->write_seq, seq0);
    }

    irt_point_config_free(&batch);
    return 0;
}

void indurtdb_update_heartbeat(void) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) return;
    irt_sub_update_heartbeat(&g_rtdb.sub, (int32_t)getpid());
}

/* ---- 校验/统计/错误 ---- */

int indurtdb_validate_id(uint32_t id) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) return 0;
    return irt_pm_validate_id(&g_rtdb.pm, id) ? 1 : 0;
}
int indurtdb_check_timeouts(uint64_t timeout_ns) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) {
        set_error("not initialized"); return -1;
    }
    return irt_pm_check_timeouts(&g_rtdb.pm, timeout_ns);
}
uint64_t indurtdb_get_write_count(void) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) return 0;
    return irt_pm_write_count(&g_rtdb.pm);
}
uint64_t indurtdb_get_timeout_count(void) {
    if (!__atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE)) return 0;
    irt_header_t* hdr = irt_shm_header(&g_rtdb.shm);
    return hdr ? __atomic_load_n(&hdr->stats.timeouts, __ATOMIC_RELAXED) : 0;
}
const char* indurtdb_get_last_error(void) {
    return g_last_error;
}
