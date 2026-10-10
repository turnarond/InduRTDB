/**
 * @file indurtdb.hpp
 * @brief InduRTDB C++ 主 API
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include "../core/subscription_manager_interface.hpp"

namespace indurtdb {

// 从 core 命名空间导出回调类型
using core::SubscriptionCallback;

class InduRTDB {
public:
    class Impl;

    static InduRTDB& instance();

    bool initialize(const char* instance_id,
                    uint32_t max_points = 10000,
                    uint32_t max_subscribers = 32);

    // 模板写入（bool, int32_t, double）
    template<typename T>
    bool write(PointId id, const T& value);

    // 字符串写入 —— 非模板重载，避免字面量类型推导问题
    bool write(PointId id, const char* value);

    // 读取
    bool read(PointId id, PointData& out) const;
    const PointData* peek(PointId id) const;

    // 订阅
    bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
    bool unsubscribe(PointId id);

    // 配置
    bool load_config(const char* config_path);

    // 心跳
    void update_heartbeat();

    // 状态
    bool is_initialized() const;
    uint64_t get_write_count() const;

    /**
     * @brief 累计成功标记的超时次数
     * @return 超时标记累计值；未初始化时返回 0
     */
    uint64_t get_timeout_count() const;

    /**
     * @brief 将点位标记为数据超时，并累加超时计数
     * @param id 点位标识符
     * @return true 成功；false 表示 id 越界或存在并发写入冲突
     * @note  超时阈值由驱动层掌握，库不做后台巡检
     */
    bool mark_timeout(PointId id);

    /**
     * @brief 当前订阅者数量
     * @return 订阅槽位占用数；未初始化时返回 0
     */
    uint64_t get_subscriber_count() const;

    void shutdown();

private:
    InduRTDB();
    ~InduRTDB();

    InduRTDB(const InduRTDB&) = delete;
    InduRTDB& operator=(const InduRTDB&) = delete;

    Impl* impl_;
};

} // namespace indurtdb
