/**
 * @file indurtdb_c_impl.cpp
 * @brief C ABI实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/api/c/indurtdb_c.h"

// 空实现，待后续完成
extern "C" {

int indurtdb_initialize(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers) {
    (void)instance_id;
    (void)max_points;
    (void)max_subscribers;
    return 0; // 返回0表示成功，非0表示失败
}

void indurtdb_shutdown() {
    // 空实现
}

int indurtdb_write_bool(uint32_t id, bool value) {
    (void)id;
    (void)value;
    return -1; // 返回非0表示失败
}

int indurtdb_write_int32(uint32_t id, int32_t value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_write_double(uint32_t id, double value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_write_string(uint32_t id, const char* value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_read_bool(uint32_t id, bool* value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_read_int32(uint32_t id, int32_t* value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_read_double(uint32_t id, double* value) {
    (void)id;
    (void)value;
    return -1;
}

int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size) {
    (void)id;
    (void)buffer;
    (void)buffer_size;
    return -1;
}

int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data) {
    (void)id;
    (void)point_data;
    return -1;
}

int indurtdb_validate_id(uint32_t id) {
    (void)id;
    return -1;
}

uint64_t indurtdb_get_write_count() {
    return 0;
}

uint64_t indurtdb_get_timeout_count() {
    return 0;
}

const char* indurtdb_get_last_error() {
    return "Not implemented";
}

} // extern "C"