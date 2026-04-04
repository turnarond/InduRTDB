/**
 * @file shared_memory_base.cpp
 * @brief 共享内存接口基类实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/osal/interface.hpp"
#include <string>

namespace indurtdb {
namespace osal {

class SharedMemoryBase : public ISharedMemory {
public:
    SharedMemoryBase() = default;
    
    virtual ~SharedMemoryBase() = default;
    
    virtual bool create(const std::string& name, size_t size) {
        (void)name;
        (void)size;
        return false;
    }
    
    virtual bool open(const std::string& name) {
        (void)name;
        return false;
    }
    
    virtual void close() {
        // 待实现
    }
    
    virtual void* get_address() const {
        return nullptr;
    }
    
    virtual size_t get_size() const {
        return 0;
    }
};

} // namespace osal
} // namespace indurtdb
