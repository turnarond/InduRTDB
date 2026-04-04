/**
 * @file time_base.cpp
 * @brief 时间接口基类实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/osal/interface.hpp"

namespace indurtdb {
namespace osal {

class TimeBase : public ITime {
public:
    TimeBase() = default;
    
    virtual ~TimeBase() = default;
    
    virtual uint64_t get_time_ns() const {
        return 0;
    }
    
    virtual void sleep_ns(uint64_t ns) const {
        (void)ns;
    }
};

} // namespace osal
} // namespace indurtdb
