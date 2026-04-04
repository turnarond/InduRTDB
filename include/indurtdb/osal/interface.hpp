/**
 * @file interface.hpp
 * @brief OS抽象层接口定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "../types/basic_types.hpp"
#include <cstddef>
#include <cstdint>

namespace indurtdb {
namespace osal {

// 共享内存接口
class ISharedMemory {
public:
    virtual ~ISharedMemory() = default;
    
    virtual void* map(std::size_t size) = 0;
    virtual void unmap() = 0;
    virtual bool is_owner() const = 0;
    virtual std::size_t size() const = 0;
    
    ISharedMemory(const ISharedMemory&) = delete;
    ISharedMemory& operator=(const ISharedMemory&) = delete;
    
protected:
    ISharedMemory() = default;
};

// 时间接口
class ITime {
public:
    virtual ~ITime() = default;
    
    virtual TimestampNs now_ns() const = 0;
    virtual void sleep_ns(TimestampNs duration) const = 0;
    
protected:
    ITime() = default;
};

// 线程接口
class IThreading {
public:
    virtual ~IThreading() = default;
    
    virtual void set_affinity(int cpu_core) = 0;
    virtual void yield() = 0;
    
protected:
    IThreading() = default;
};

// 通知接口
class INotification {
public:
    virtual ~INotification() = default;
    
    virtual bool send(const void* data, std::size_t size) = 0;
    virtual bool receive(void* data, std::size_t size, TimestampNs timeout_ns) = 0;
    
protected:
    INotification() = default;
};

} // namespace osal
} // namespace indurtdb