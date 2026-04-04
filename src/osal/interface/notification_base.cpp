/**
 * @file notification_base.cpp
 * @brief 通知接口基类实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/osal/interface.hpp"
#include <string>

namespace indurtdb {
namespace osal {

class NotificationBase : public INotification {
public:
    NotificationBase() = default;
    
    virtual ~NotificationBase() = default;
    
    virtual bool create(const std::string& name) {
        (void)name;
        return false;
    }
    
    virtual bool open(const std::string& name) {
        (void)name;
        return false;
    }
    
    virtual void close() {
        // 待实现
    }
    
    virtual bool send(const void* data, size_t size) {
        (void)data;
        (void)size;
        return false;
    }
    
    virtual bool receive(void* data, size_t size, uint64_t timeout_ns) {
        (void)data;
        (void)size;
        (void)timeout_ns;
        return false;
    }
};

} // namespace osal
} // namespace indurtdb
