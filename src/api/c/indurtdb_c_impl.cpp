/**
 * @file indurtdb_c_impl.cpp
 * @brief C ABI 实现 —— 桥接到 C++ InduRTDB
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#include <indurtdb/api/c/indurtdb_c.h>
#include <indurtdb/api/indurtdb.hpp>
#include <cstring>

extern "C" {

// ---- 初始化/关闭 ----

int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers) {
    auto& db = indurtdb::InduRTDB::instance();
    return db.initialize(instance_id, max_points, max_subscribers) ? 0 : -1;
}

void indurtdb_shutdown() {
    indurtdb::InduRTDB::instance().shutdown();
}

// ---- 写入 ----

int indurtdb_write_bool(uint32_t id, bool value) {
    return indurtdb::InduRTDB::instance().write(id, value) ? 0 : -1;
}

int indurtdb_write_int32(uint32_t id, int32_t value) {
    return indurtdb::InduRTDB::instance().write(id, value) ? 0 : -1;
}

int indurtdb_write_double(uint32_t id, double value) {
    return indurtdb::InduRTDB::instance().write(id, value) ? 0 : -1;
}

int indurtdb_write_string(uint32_t id, const char* value) {
    return indurtdb::InduRTDB::instance().write(id, value) ? 0 : -1;
}

// ---- 读取 ----

int indurtdb_read_bool(uint32_t id, bool* value) {
    if (!value) return -1;
    indurtdb::PointData p;
    if (!indurtdb::InduRTDB::instance().read(id, p)) return -1;
    *value = p.value.b;
    return 0;
}

int indurtdb_read_int32(uint32_t id, int32_t* value) {
    if (!value) return -1;
    indurtdb::PointData p;
    if (!indurtdb::InduRTDB::instance().read(id, p)) return -1;
    *value = p.value.i;
    return 0;
}

int indurtdb_read_double(uint32_t id, double* value) {
    if (!value) return -1;
    indurtdb::PointData p;
    if (!indurtdb::InduRTDB::instance().read(id, p)) return -1;
    *value = p.value.d;
    return 0;
}

int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size) {
    if (!buffer || buffer_size == 0) return -1;
    indurtdb::PointData p;
    if (!indurtdb::InduRTDB::instance().read(id, p)) return -1;
    std::strncpy(buffer, p.value.str, buffer_size - 1);
    buffer[buffer_size - 1] = '\0';
    return 0;
}

int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data) {
    if (!point_data) return -1;
    indurtdb::PointData p;
    if (!indurtdb::InduRTDB::instance().read(id, p)) return -1;
    // C 结构体与 C++ 结构体布局一致，直接 memcpy
    std::memcpy(point_data, &p, sizeof(indurtdb_point_t));
    return 0;
}

// ---- 验证 ----

int indurtdb_validate_id(uint32_t id) {
    // 通过读操作间接验证
    indurtdb::PointData dummy;
    return indurtdb::InduRTDB::instance().read(id, dummy) ? 0 : -1;
}

// ---- 统计 ----

uint64_t indurtdb_get_write_count() {
    return indurtdb::InduRTDB::instance().get_write_count();
}

uint64_t indurtdb_get_timeout_count() {
    return 0;
}

// ---- 错误处理 ----

static thread_local char g_last_error[256] = {0};

const char* indurtdb_get_last_error() {
    return g_last_error[0] ? g_last_error : "Success";
}

} // extern "C"
