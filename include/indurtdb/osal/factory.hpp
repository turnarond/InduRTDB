/**
 * @file factory.hpp
 * @brief OSAL工厂函数声明
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "interface.hpp"
#include <memory>
#include <string>

namespace indurtdb {
namespace osal {

class OSALFactory {
public:
    static std::unique_ptr<ISharedMemory> create_shared_memory(const std::string& name);
    static std::unique_ptr<ITime> create_time();
    static std::unique_ptr<IThreading> create_threading();
    static std::unique_ptr<INotification> create_notification(const std::string& path, bool as_server);
    
    static const char* platform_name();
    static bool is_platform_supported();
};

} // namespace osal
} // namespace indurtdb