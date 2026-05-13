/**
 * @file factory.cpp
 * @brief OSAL 工厂实现 —— 直接实例化平台具体类
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#include <indurtdb/osal/factory.hpp>
#include <cstring>
#include <cstdio>
#include <string>
#include <new>

// 前向声明 POSIX 具体实现类
namespace indurtdb {
namespace osal {
namespace posix {
    class SharedMemory;
    class Time;
    class Threading;
    class Notification;
}
}
}

// POSIX 实现中定义的工厂方法（非 extern "C"，C++ 链接）
namespace indurtdb {
namespace osal {
namespace posix {

ISharedMemory* create_shared_memory(const std::string& name);
void destroy_shared_memory(ISharedMemory* mem);

ITime* create_time();
void destroy_time(ITime* time);

IThreading* create_threading();
void destroy_threading(IThreading* threading);

INotification* create_notification(const std::string& path, bool as_server);
void destroy_notification(INotification* notification);

} // namespace posix
} // namespace osal
} // namespace indurtdb

namespace indurtdb {
namespace osal {

// ---- 工厂函数实现 ----

std::unique_ptr<ISharedMemory> OSALFactory::create_shared_memory(
    const std::string& name) {
    ISharedMemory* mem = posix::create_shared_memory(name);
    return std::unique_ptr<ISharedMemory>(mem);
}

std::unique_ptr<ITime> OSALFactory::create_time() {
    ITime* time = posix::create_time();
    return std::unique_ptr<ITime>(time);
}

std::unique_ptr<IThreading> OSALFactory::create_threading() {
    IThreading* threading = posix::create_threading();
    return std::unique_ptr<IThreading>(threading);
}

std::unique_ptr<INotification> OSALFactory::create_notification(
    const std::string& path, bool as_server) {
    INotification* notification = posix::create_notification(path, as_server);
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
