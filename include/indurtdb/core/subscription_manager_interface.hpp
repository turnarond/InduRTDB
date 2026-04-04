/**
 * @file subscription_manager_interface.hpp
 * @brief 订阅管理接口定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "point_manager_interface.hpp"
#include <indurtdb/osal/interface.hpp>
#include <functional>
#include <memory>

namespace indurtdb {
namespace core {

/**
 * @brief 订阅回调类型定义
 * 
 * 当点位数据发生变化时，将调用该回调函数
 * 
 * @param point 变化后的点位数据
 */
using SubscriptionCallback = std::function<void(const PointData&)>;

/**
 * @brief 订阅信息结构
 */
struct SubscriptionInfo {
    PointId id;                          ///< 点位ID
    SubscriptionCallback callback;       ///< 回调函数
    TimestampNs last_heartbeat_ns;       ///< 最后心跳时间（纳秒）
    bool active;                         ///< 是否活跃
};

/**
 * @brief 订阅管理器接口
 * 
 * 负责管理点位数据变化的订阅和通知
 * 支持并发安全的订阅管理和通知发送
 */
class ISubscriptionManager {
public:
    virtual ~ISubscriptionManager() = default;

    /**
     * @brief 订阅点位数据变化
     * 
     * @param id 点位ID
     * @param callback 回调函数
     * @return true 订阅成功
     * @return false 订阅失败（ID无效或回调为空）
     */
    virtual bool subscribe(PointId id, SubscriptionCallback callback) = 0;

    /**
     * @brief 取消订阅点位数据变化
     * 
     * @param id 点位ID
     * @return true 取消成功
     * @return false 取消失败（未找到该点位的订阅）
     */
    virtual bool unsubscribe(PointId id) = 0;

    /**
     * @brief 通知所有订阅者点位数据变化
     * 
     * @param id 点位ID
     * @param data 变化后的点位数据
     * @return true 通知成功
     * @return false 通知失败（无订阅者或ID无效）
     */
    virtual bool notify(PointId id, const PointData& data) = 0;

    /**
     * @brief 更新订阅者的心跳时间
     * 
     * @param id 点位ID
     * @return true 更新成功
     * @return false 更新失败（未找到该点位的订阅）
     */
    virtual bool update_heartbeat(PointId id) = 0;

    /**
     * @brief 清理超时的订阅
     * 
     * @param timeout_ns 超时时间（纳秒）
     * @return std::size_t 清理的超时订阅数量
     */
    virtual std::size_t cleanup_timeout_subscriptions(TimestampNs timeout_ns) = 0;

    /**
     * @brief 获取订阅数量
     * 
     * @return std::size_t 总订阅数量
     */
    virtual std::size_t get_subscription_count() const = 0;

    /**
     * @brief 获取特定点位的订阅数量
     * 
     * @param id 点位ID
     * @return std::size_t 该点位的订阅数量
     */
    virtual std::size_t get_subscription_count(PointId id) const = 0;

    /**
     * @brief 检查点位是否有订阅
     * 
     * @param id 点位ID
     * @return true 有点位有订阅
     * @return false 点位无订阅
     */
    virtual bool has_subscriptions(PointId id) const = 0;

    /**
     * @brief 清理所有订阅
     * 
     * @return true 清理成功
     * @return false 清理失败（无订阅可清理）
     */
    virtual bool clear_all_subscriptions() = 0;

    /**
     * @brief 验证订阅管理器的状态
     * 
     * @return true 状态正常
     * @return false 状态异常
     */
    virtual bool validate() const = 0;
};

/**
 * @brief 订阅管理器工厂函数
 * 
 * @return std::unique_ptr<ISubscriptionManager> 订阅管理器实例
 */
std::unique_ptr<ISubscriptionManager> create_subscription_manager(
    const std::shared_ptr<osal::ITime>& time_provider);

} // namespace core
} // namespace indurtdb
