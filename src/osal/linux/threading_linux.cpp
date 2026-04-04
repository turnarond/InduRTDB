/**
 * @file threading_linux.cpp
 * @brief Linux平台线程管理封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/threading_posix.cpp"

namespace indurtdb {
namespace osal {
namespace linux {

class Threading : public posix::Threading {
public:
    Threading() = default;
    ~Threading() override = default;
};

} // namespace linux
} // namespace osal
} // namespace indurtdb