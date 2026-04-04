/**
 * @file memory_layout.hpp
 * @brief 共享内存布局定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "basic_types.hpp"
#include <type_traits>

namespace indurtdb {

// 共享内存头部结构（64字节对齐）
struct InduRTDBHeader {
    uint32_t magic;         // 魔术字 0x1DBA1DBA
    uint32_t version;       // 版本号
    uint32_t max_points;    // 最大点位数
    uint32_t max_subscribers; // 最大订阅者数
    uint64_t write_seq;     // Seqlock序列号
    struct {
        uint64_t writes;    // 总写入次数
        uint64_t timeouts;  // 超时点位计数
    } stats;
} __attribute__((packed, aligned(64)));

static_assert(std::is_pod_v<InduRTDBHeader>, "Header must be POD");
static_assert(sizeof(InduRTDBHeader) == 64, "Header size must be 64 bytes");

// 点位数据结构（128字节对齐）
struct PointData {
    union Value {
        bool b;            // 布尔值
        int32_t i;         // 整数值
        double d;          // 浮点值
        char str[32];      // 字符串值（最大31字符+终止符）
    } value;
    
    TimestampNs timestamp_ns; // 时间戳（纳秒）
    PointType type;           // 数据类型
    Quality quality;          // 数据质量
    Unit unit;                // 单位
    Access access;            // 访问权限
    char name[64];            // 点位名称
    
    uint8_t padding[19];      // 填充到128字节
} __attribute__((packed, aligned(128)));

static_assert(std::is_pod_v<PointData>, "PointData must be POD");
static_assert(sizeof(PointData) == 128, "PointData size must be 128 bytes");

// 订阅者条目结构（16字节对齐）
struct SubscriberEntry {
    Pid pid;                  // 进程ID
    TimestampNs last_heartbeat_ns; // 最后心跳时间
    
    uint8_t padding[4];       // 填充到16字节
} __attribute__((packed, aligned(16)));

static_assert(std::is_pod_v<SubscriberEntry>, "SubscriberEntry must be POD");
static_assert(sizeof(SubscriberEntry) == 16, "SubscriberEntry size must be 16 bytes");

} // namespace indurtdb