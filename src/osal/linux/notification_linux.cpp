/**
 * @file notification_linux.cpp
 * @brief Linux平台通知机制封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/notification_posix.cpp"

namespace indurtdb {
namespace osal {
namespace linux {

class Notification : public posix::Notification {
public:
    Notification(const std::string& path, bool as_server) 
        : posix::Notification(path, as_server) {}
    
    ~Notification() override = default;
};

} // namespace linux
} // namespace osal
} // namespace indurtdb