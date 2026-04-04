/**
 * @file time_sylixos.cpp
 * @brief SylixOS平台时间管理封装（直接使用POSIX实现）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "indurtdb/osal/interface.hpp"
#include "../posix/time_posix.cpp"

namespace indurtdb {
namespace osal {
namespace sylixos {

class Time : public posix::Time {
public:
    Time() = default;
    ~Time() override = default;
};

extern "C" ITime* create_time() {
    return posix::create_time();
}

extern "C" void destroy_time(ITime* time) {
    posix::destroy_time(time);
}

} // namespace sylixos
} // namespace osal
} // namespace indurtdb