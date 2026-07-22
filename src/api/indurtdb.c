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
#include <string.h>
#include <stdio.h>
#include <unistd.h>

/* ---- 全局单例 ---- */
static struct {
    irt_shm_t shm;
    irt_pm_t  pm;
    irt_sub_t sub;
    bool      initialized;
    char      last_error[256];
} g_rtdb;

static void set_error(const char* msg) {
    snprintf(g_rtdb.last_error, sizeof(g_rtdb.last_error), "%s", msg);
}

/* ---- 生命周期 ---- */

int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points, uint32_t max_subscribers) {
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
    g_rtdb.initialized = true;
    return 0;
}

void indurtdb_shutdown(void) {
    if (!g_rtdb.initialized) return;
    irt_shm_shutdown(&g_rtdb.shm);
    memset(&g_rtdb, 0, sizeof(g_rtdb));
}

bool indurtdb_is_initialized(void) {
    return __atomic_load_n(&g_rtdb.initialized, __ATOMIC_ACQUIRE);
}

/* ---- 写入 ---- */

/* 写入成功后通知订阅者 (与 v2.x InduRTDB::write 行为一致) */
static int write_and_notify(int rc, uint32_t id) {
    if (rc == 0) {
        const indurtdb_point_t* p = irt_pm_peek(&g_rtdb.pm, id);
        if (p) irt_sub_notify(&g_rtdb.sub, id, p);
    }
    return rc;
}

#define ENSURE_INIT() do { \
    if (!g_rtdb.initialized) { set_error("not initialized"); return -1; } \
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
    if (!g_rtdb.initialized) { set_error("not initialized"); return NULL; }
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
        if (irt_pm_write_bool(&g_rtdb.pm, start_id + i, values[i]) != 0)
            return i;
    }
    return count;
}
int indurtdb_write_range_int32(uint32_t start_id, const int32_t* values,
                                uint16_t count) {
    ENSURE_INIT();
    if (!values) { set_error("null values pointer"); return -1; }
    for (uint16_t i = 0; i < count; i++) {
        if (irt_pm_write_int32(&g_rtdb.pm, start_id+i, values[i]) != 0)
            return i;
    }
    return count;
}
int indurtdb_write_range_double(uint32_t start_id, const double* values,
                                 uint16_t count) {
    ENSURE_INIT();
    if (!values) { set_error("null values pointer"); return -1; }
    for (uint16_t i = 0; i < count; i++) {
        if (irt_pm_write_double(&g_rtdb.pm, start_id+i, values[i]) != 0)
            return i;
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
    if (g_rtdb.initialized) { set_error("already initialized"); return -1; }
    irt_config_t cfg;
    irt_config_init_defaults(&cfg);
    if (irt_config_load_file(&cfg, config_path) != 0) {
        set_error("config file read failed");
        return -1;
    }
    return indurtdb_initialize(cfg.instance_id, cfg.max_points,
                               cfg.max_subscribers);
}

void indurtdb_update_heartbeat(void) {
    if (!g_rtdb.initialized) return;
    irt_sub_update_heartbeat(&g_rtdb.sub, (int32_t)getpid());
}

/* ---- 校验/统计/错误 ---- */

int indurtdb_validate_id(uint32_t id) {
    if (!g_rtdb.initialized) return 0;
    return irt_pm_validate_id(&g_rtdb.pm, id) ? 1 : 0;
}
uint64_t indurtdb_get_write_count(void) {
    if (!g_rtdb.initialized) return 0;
    return irt_pm_write_count(&g_rtdb.pm);
}
uint64_t indurtdb_get_timeout_count(void) {
    if (!g_rtdb.initialized) return 0;
    irt_header_t* hdr = irt_shm_header(&g_rtdb.shm);
    return hdr ? __atomic_load_n(&hdr->stats.timeouts, __ATOMIC_RELAXED) : 0;
}
const char* indurtdb_get_last_error(void) {
    return g_rtdb.last_error;
}
