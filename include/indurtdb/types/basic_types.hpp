/**
 * @file basic_types.hpp
 * @brief 基础类型定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include <cstdint>
#include <cstddef>

namespace indurtdb {

// 基础类型别名
using PointId = uint32_t;
using TimestampNs = uint64_t;
using Pid = int32_t;

// 数据类型枚举
enum class PointType : uint8_t {
    BOOL = 0,
    INT32 = 1,
    DOUBLE = 2,
    STRING = 3
};

// 数据质量枚举
enum class Quality : uint8_t {
    GOOD = 0,
    BAD = 1,
    TIMEOUT = 2,
    SUBSTITUTED = 3
};

// 访问权限枚举
enum class Access : uint8_t {
    READ_ONLY = 1,
    READ_WRITE = 3
};

// 单位枚举
enum class Unit : uint16_t {
    NO_UNIT = 0,
    DEGREES_CELSIUS = 1,
    PASCAL = 2,
    PERCENT = 3
};

} // namespace indurtdb