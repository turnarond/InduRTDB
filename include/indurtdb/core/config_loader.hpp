/**
 * @file config_loader.hpp
 * @brief 轻量级 YAML 配置解析器（零第三方依赖）
 * @version 2.0.0
 * @date 2026-05-11
 */

#pragma once

#include "../types/basic_types.hpp"
#include <cstddef>

namespace indurtdb {
namespace core {

// 解析出的单个点位配置
struct PointConfig {
    PointId  id;
    uint8_t  type;    // 0=bool, 1=int32, 2=double, 3=string
    uint16_t unit;
    uint8_t  access;
    char     name[64];
};

// 配置解析结果
struct ConfigResult {
    PointConfig* points;
    size_t       count;
    size_t       capacity;
};

/**
 * @brief 解析 YAML 点位配置文件
 *
 * 支持格式（SRS 定义）：
 *   points:
 *     - id: 1001
 *       name: "AHU_01.Supply_Temp"
 *       type: double
 *       unit: 1
 *       access: 1
 *
 * @param path 配置文件路径
 * @param out  解析结果（调用方负责释放 out.points）
 * @return true 解析成功
 */
bool parse_point_config(const char* path, ConfigResult& out);

/**
 * @brief 释放 ConfigResult
 */
void free_config_result(ConfigResult& result);

/**
 * @brief 将 type 字符串转换为 PointType 枚举
 */
uint8_t parse_type_string(const char* s);

} // namespace core
} // namespace indurtdb
