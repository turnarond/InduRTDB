/**
 * @file threading_sylixos.cpp
 * @brief SylixOS平台线程管理封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/threading_posix.cpp"

namespace indurtdb {
namespace osal {
namespace sylixos {

class Threading : public posix::Threading {
public:
    Threading() = default;
    ~Threading() override = default;
};

extern "C" IThreading* create_threading() {
    return posix::create_threading();
}

extern "C" void destroy_threading(IThreading* threading) {
    posix::destroy_threading(threading);
}

} // namespace sylixos
} // namespace osal
} // namespace indurtdb