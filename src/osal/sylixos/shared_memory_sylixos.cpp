/**
 * @file shared_memory_sylixos.cpp
 * @brief SylixOS平台共享内存封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/shared_memory_posix.cpp"

namespace indurtdb {
namespace osal {
namespace sylixos {

class SharedMemory : public posix::SharedMemory {
public:
    SharedMemory(const std::string& name) 
        : posix::SharedMemory(name) {}
    
    ~SharedMemory() override = default;
};

extern "C" ISharedMemory* create_shared_memory(const char* name) {
    return posix::create_shared_memory(name);
}

extern "C" void destroy_shared_memory(ISharedMemory* mem) {
    posix::destroy_shared_memory(mem);
}

} // namespace sylixos
} // namespace osal
} // namespace indurtdb