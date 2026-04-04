/**
 * @file subscription_manager.cpp
 * @brief 订阅管理器实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <indurtdb/core/subscription_manager_interface.hpp>
#include <indurtdb/osal/factory.hpp>
#include <unordered_map>
#include <vector>
#include <mutex>
#include <algorithm>

namespace indurtdb {
namespace core {

/**
 * @brief 订阅管理器实现类
 * 
 * 线程安全的订阅管理器，支持并发访问和通知
 */
class SubscriptionManager : public ISubscriptionManager {
public:
    explicit SubscriptionManager(const std::shared_ptr<osal::ITime>& time_provider)
        : time_provider_(time_provider) {
        if (!time_provider_) {
            time_provider_ = std::shared_ptr<osal::ITime>(osal::OSALFactory::create_time().release());
        }
    }

    ~SubscriptionManager() override = default;

    bool subscribe(PointId id, SubscriptionCallback callback) override {
        if (!callback) {
            return false;
        }

        std::lock_guard<std::mutex> lock(mutex_);

        auto& subscriptions = subscriptions_[id];
        // 不再检查重复订阅，直接添加
        (void)subscriptions;

        SubscriptionInfo info;
        info.id = id;
        info.callback = std::move(callback);
        info.last_heartbeat_ns = time_provider_ ? time_provider_->now_ns() : 0;
        info.active = true;

        subscriptions.push_back(std::move(info));
        return true;
    }

    bool unsubscribe(PointId id) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return false;
        }

        subscriptions_.erase(it);
        return true;
    }

    bool notify(PointId id, const PointData& data) override {
        std::vector<SubscriptionInfo> active_subscriptions;

        {
            std::lock_guard<std::mutex> lock(mutex_);
            auto it = subscriptions_.find(id);
            if (it == subscriptions_.end()) {
                return false;
            }

            active_subscriptions = it->second;
        }

        for (const auto& info : active_subscriptions) {
            if (info.active && info.callback) {
                info.callback(data);
            }
        }

        return true;
    }

    bool update_heartbeat(PointId id) override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return false;
        }

        for (auto& info : it->second) {
            if (info.active && time_provider_) {
                info.last_heartbeat_ns = time_provider_->now_ns();
            }
        }

        return true;
    }

    std::size_t cleanup_timeout_subscriptions(TimestampNs timeout_ns) override {
        std::lock_guard<std::mutex> lock(mutex_);

        TimestampNs now = time_provider_ ? time_provider_->now_ns() : 0;
        std::size_t cleaned_count = 0;

        auto it = subscriptions_.begin();
        while (it != subscriptions_.end()) {
            auto& infos = it->second;
            
            auto erase_it = std::remove_if(infos.begin(), infos.end(),
                [this, now, timeout_ns](const SubscriptionInfo& info) {
                    if (!info.active) {
                        return true;
                    }

                    if (now > 0 && now - info.last_heartbeat_ns > timeout_ns) {
                        return true;
                    }

                    return false;
                });

            cleaned_count += static_cast<std::size_t>(std::distance(erase_it, infos.end()));
            infos.erase(erase_it, infos.end());

            if (infos.empty()) {
                it = subscriptions_.erase(it);
            } else {
                ++it;
            }
        }

        return cleaned_count;
    }

    std::size_t get_subscription_count() const override {
        std::lock_guard<std::mutex> lock(mutex_);

        std::size_t count = 0;
        for (const auto& pair : subscriptions_) {
            count += pair.second.size();
        }

        return count;
    }

    std::size_t get_subscription_count(PointId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return 0;
        }

        return it->second.size();
    }

    bool has_subscriptions(PointId id) const override {
        std::lock_guard<std::mutex> lock(mutex_);

        auto it = subscriptions_.find(id);
        if (it == subscriptions_.end()) {
            return false;
        }

        return !it->second.empty();
    }

    bool clear_all_subscriptions() override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (subscriptions_.empty()) {
            return false;
        }

        subscriptions_.clear();
        return true;
    }

    bool validate() const override {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!time_provider_) {
            return false;
        }

        for (const auto& pair : subscriptions_) {
            if (pair.first == 0) {
                return false;
            }

            for (const auto& info : pair.second) {
                if (!info.callback) {
                    return false;
                }

                if (info.last_heartbeat_ns == 0) {
                    return false;
                }
            }
        }

        return true;
    }

private:
    mutable std::mutex mutex_;
    std::shared_ptr<osal::ITime> time_provider_;
    std::unordered_map<PointId, std::vector<SubscriptionInfo>> subscriptions_;
};

std::unique_ptr<ISubscriptionManager> create_subscription_manager(
    const std::shared_ptr<osal::ITime>& time_provider) {
    return std::unique_ptr<ISubscriptionManager>(new (std::nothrow) SubscriptionManager(time_provider));
}

} // namespace core
} // namespace indurtdb
