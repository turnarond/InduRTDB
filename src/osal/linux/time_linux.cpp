/**
 * @file time_linux.cpp
 * @brief Linux平台时间管理封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/time_posix.cpp"

namespace indurtdb {
namespace osal {
namespace linux {

class Time : public posix::Time {
public:
    Time() = default;
    ~Time() override = default;
};

} // namespace linux
} // namespace osal
} // namespace indurtdb