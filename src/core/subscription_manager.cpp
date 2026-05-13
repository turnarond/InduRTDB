/**
 * @file subscription_manager.cpp
 * @brief SubscriptionManager 实现 —— 定长数组，零 STL
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#include <indurtdb/core/subscription_manager_interface.hpp>
#include <cstring>

namespace indurtdb {
namespace core {

SubscriptionManager::SubscriptionManager(
    osal::ITime* time,
    SubscriberEntry* shm_sub_table,
    uint32_t max_subscribers)
    : slot_count_(0)
    , time_(time)
    , shm_sub_table_(shm_sub_table)
    , max_subscribers_(max_subscribers)
{
    // 初始化所有槽位为非活跃
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        slots_[i].active = false;
    }
}

bool SubscriptionManager::subscribe(PointId id, SubscriptionCallback cb,
                                     void* user_data) {
    if (!cb) return false;
    if (slot_count_ >= MAX_CALLBACKS) return false;

    // 线性查找空闲槽位
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (!slots_[i].active) {
            slots_[i].point_id           = id;
            slots_[i].callback           = cb;
            slots_[i].user_data          = user_data;
            slots_[i].last_heartbeat_ns  = time_ ? time_->now_ns() : 0;
            slots_[i].active             = true;
            slot_count_++;
            return true;
        }
    }
    return false; // 槽位满
}

bool SubscriptionManager::unsubscribe(PointId id) {
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (slots_[i].active && slots_[i].point_id == id) {
            slots_[i].active = false;
            slot_count_--;
            return true;
        }
    }
    return false;
}

void SubscriptionManager::notify(PointId id, const PointData& data) {
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (slots_[i].active && slots_[i].point_id == id) {
            if (slots_[i].callback) {
                slots_[i].callback(id, data, slots_[i].user_data);
            }
        }
    }
}

void SubscriptionManager::update_heartbeat(Pid pid) {
    if (!shm_sub_table_) return;

    // 在共享内存心跳表中查找/更新
    for (uint32_t i = 0; i < max_subscribers_; ++i) {
        if (shm_sub_table_[i].pid == pid) {
            shm_sub_table_[i].last_heartbeat_ns =
                time_ ? time_->now_ns() : 0;
            return;
        }
    }

    // 新订阅者，注册到空闲槽位
    for (uint32_t i = 0; i < max_subscribers_; ++i) {
        if (shm_sub_table_[i].pid == 0) {
            shm_sub_table_[i].pid = pid;
            shm_sub_table_[i].last_heartbeat_ns =
                time_ ? time_->now_ns() : 0;
            return;
        }
    }
}

size_t SubscriptionManager::cleanup_zombies() {
    if (!shm_sub_table_ || !time_) return 0;

    uint64_t now = time_->now_ns();
    size_t cleaned = 0;

    for (uint32_t i = 0; i < max_subscribers_; ++i) {
        if (shm_sub_table_[i].pid == 0) continue;

        // 心跳超时 > 1秒
        if (now - shm_sub_table_[i].last_heartbeat_ns > 1'000'000'000ULL) {
            shm_sub_table_[i].pid = 0;
            shm_sub_table_[i].last_heartbeat_ns = 0;
            cleaned++;
        }
    }
    return cleaned;
}

size_t SubscriptionManager::subscription_count() const {
    return slot_count_;
}

size_t SubscriptionManager::subscription_count(PointId id) const {
    size_t count = 0;
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (slots_[i].active && slots_[i].point_id == id) {
            count++;
        }
    }
    return count;
}

bool SubscriptionManager::has_subscriptions(PointId id) const {
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (slots_[i].active && slots_[i].point_id == id) {
            return true;
        }
    }
    return false;
}

void SubscriptionManager::clear_all() {
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        slots_[i].active = false;
    }
    slot_count_ = 0;
}

bool SubscriptionManager::validate() const {
    if (!time_) return false;

    size_t active_count = 0;
    for (size_t i = 0; i < MAX_CALLBACKS; ++i) {
        if (slots_[i].active) {
            if (!slots_[i].callback) return false;
            active_count++;
        }
    }
    return active_count == slot_count_;
}

} // namespace core
} // namespace indurtdb
