/**
 * @file indurtdb_c.h
 * @brief C API interface for InduRTDB
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#ifndef INDURTDB_API_C_INDURTDB_C_H_
#define INDURTDB_API_C_INDURTDB_C_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// C API类型定义
typedef struct {
    union {
        bool b;
        int32_t i;
        double d;
        char str[32];
    } value;
    uint64_t timestamp_ns;
    uint8_t type;
    uint8_t quality;
    uint16_t unit;
    uint8_t access;
    char name[64];
    uint8_t padding[19];
} indurtdb_point_t;

// 初始化函数
int indurtdb_initialize(const char* instance_id,
                       uint32_t max_points,
                       uint32_t max_subscribers);

// 关闭函数
void indurtdb_shutdown();

// 写入函数
int indurtdb_write_bool(uint32_t id, bool value);
int indurtdb_write_int32(uint32_t id, int32_t value);
int indurtdb_write_double(uint32_t id, double value);
int indurtdb_write_string(uint32_t id, const char* value);

// 读取函数
int indurtdb_read_bool(uint32_t id, bool* value);
int indurtdb_read_int32(uint32_t id, int32_t* value);
int indurtdb_read_double(uint32_t id, double* value);
int indurtdb_read_string(uint32_t id, char* buffer, size_t buffer_size);

// 读取完整点位数据
int indurtdb_read_point(uint32_t id, indurtdb_point_t* point_data);

// 验证函数
int indurtdb_validate_id(uint32_t id);

// 统计函数
uint64_t indurtdb_get_write_count();
uint64_t indurtdb_get_timeout_count();

// 错误处理
const char* indurtdb_get_last_error();

#ifdef __cplusplus
}
#endif

#endif  // INDURTDB_API_C_INDURTDB_C_H_
