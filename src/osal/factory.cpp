/**
 * @file factory.cpp
 * @brief OSAL工厂函数实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/factory.hpp"

// extern "C" 工厂函数声明
extern "C" {
    indurtdb::osal::ISharedMemory* create_shared_memory(const char* name);
    void destroy_shared_memory(indurtdb::osal::ISharedMemory* mem);
    
    indurtdb::osal::ITime* create_time();
    void destroy_time(indurtdb::osal::ITime* time);
    
    indurtdb::osal::IThreading* create_threading();
    void destroy_threading(indurtdb::osal::IThreading* threading);
    
    indurtdb::osal::INotification* create_notification(const char* path, bool as_server);
    void destroy_notification(indurtdb::osal::INotification* notification);
}

namespace indurtdb {
namespace osal {

// 工厂函数实现
std::unique_ptr<ISharedMemory> OSALFactory::create_shared_memory(
    const std::string& name) {
    
    ISharedMemory* mem = ::create_shared_memory(name.c_str());
    return std::unique_ptr<ISharedMemory>(mem);
}

std::unique_ptr<ITime> OSALFactory::create_time() {
    ITime* time = ::create_time();
    return std::unique_ptr<ITime>(time);
}

std::unique_ptr<IThreading> OSALFactory::create_threading() {
    IThreading* threading = ::create_threading();
    return std::unique_ptr<IThreading>(threading);
}

std::unique_ptr<INotification> OSALFactory::create_notification(
    const std::string& path, bool as_server) {
    
    INotification* notification = ::create_notification(path.c_str(), as_server);
    return std::unique_ptr<INotification>(notification);
}

const char* OSALFactory::platform_name() {
#if defined(INDURTDB_OS_LINUX)
    return "Linux";
#elif defined(INDURTDB_OS_SYLIXOS)
    return "SylixOS";
#else
    return "Unknown";
#endif
}

bool OSALFactory::is_platform_supported() {
#if defined(INDURTDB_OS_LINUX) || defined(INDURTDB_OS_SYLIXOS)
    return true;
#else
    return false;
#endif
}

} // namespace osal
} // namespace indurtdb