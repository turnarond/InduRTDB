/**
 * @file notification_sylixos.cpp
 * @brief SylixOS平台通知机制封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/notification_posix.cpp"

namespace indurtdb {
namespace osal {
namespace sylixos {

class Notification : public posix::Notification {
public:
    Notification(const std::string& path, bool as_server) 
        : posix::Notification(path, as_server) {}
    
    ~Notification() override = default;
};

extern "C" INotification* create_notification(const char* path, bool as_server) {
    return posix::create_notification(path, as_server);
}

extern "C" void destroy_notification(INotification* notification) {
    posix::destroy_notification(notification);
}

} // namespace sylixos
} // namespace osal
} // namespace indurtdb