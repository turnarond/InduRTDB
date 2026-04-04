/**
 * @file shared_memory_linux.cpp
 * @brief Linux平台共享内存封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/shared_memory_posix.cpp"

namespace indurtdb {
namespace osal {
namespace linux {

class SharedMemory : public posix::SharedMemory {
public:
    SharedMemory(const std::string& name) 
        : posix::SharedMemory(name) {}
    
    ~SharedMemory() override = default;
};

} // namespace linux
} // namespace osal
} // namespace indurtdb