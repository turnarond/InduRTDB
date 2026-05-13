/**
 * @file subscription_manager_interface.hpp
 * @brief SubscriptionManager —— 定长数组，零 STL，零堆分配
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include "../osal/interface.hpp"

namespace indurtdb {
namespace core {

// C 风格回调 —— 替代 std::function，避免堆分配
using SubscriptionCallback = void (*)(PointId id, const PointData& data,
                                       void* user_data);

// 定长订阅槽位
struct SubscriberSlot {
    PointId             point_id;
    SubscriptionCallback callback;
    void*               user_data;
    uint64_t            last_heartbeat_ns;
    bool                active;
};

class SubscriptionManager {
public:
    static constexpr size_t MAX_CALLBACKS = 256;

    /**
     * @brief 构造函数
     * @param time           OSAL 时间接口
     * @param shm_sub_table  共享内存中的 SubscriberEntry 表
     * @param max_subscribers 最大订阅者进程数
     */
    SubscriptionManager(osal::ITime* time,
                        SubscriberEntry* shm_sub_table,
                        uint32_t max_subscribers);

    // ---- 订阅管理 ----

    bool subscribe(PointId id, SubscriptionCallback cb, void* user_data);
    bool unsubscribe(PointId id);

    // ---- 通知 ----

    void notify(PointId id, const PointData& data);

    // ---- 心跳与清理 ----

    void update_heartbeat(Pid pid);
    size_t cleanup_zombies();

    // ---- 查询 ----

    size_t subscription_count() const;
    size_t subscription_count(PointId id) const;
    bool   has_subscriptions(PointId id) const;
    void   clear_all();
    bool   validate() const;

private:
    SubscriberSlot  slots_[MAX_CALLBACKS];
    size_t          slot_count_;

    osal::ITime*    time_;

    // 指向共享内存中的心跳表
    SubscriberEntry* shm_sub_table_;
    uint32_t         max_subscribers_;
};

} // namespace core
} // namespace indurtdb
