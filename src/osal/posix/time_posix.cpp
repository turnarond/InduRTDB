/**
 * @file time_posix.cpp
 * @brief POSIX时间管理实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <indurtdb/osal/interface.hpp>
#include <time.h>
#include <errno.h>
#include <new>

namespace indurtdb {
namespace osal {
namespace posix {

class Time : public ITime {
public:
    Time() = default;
    ~Time() override = default;
    
    TimestampNs now_ns() const override {
        struct timespec ts;
        
        if (clock_gettime(CLOCK_MONOTONIC_RAW, &ts) == 0) {
            return static_cast<TimestampNs>(ts.tv_sec) * 1000000000ULL + 
                   static_cast<TimestampNs>(ts.tv_nsec);
        }
        
        if (clock_gettime(CLOCK_REALTIME, &ts) == 0) {
            return static_cast<TimestampNs>(ts.tv_sec) * 1000000000ULL + 
                   static_cast<TimestampNs>(ts.tv_nsec);
        }
        
        return 0;
    }
    
    void sleep_ns(TimestampNs duration) const override {
        struct timespec req, rem;
        
        req.tv_sec = duration / 1000000000ULL;
        req.tv_nsec = duration % 1000000000ULL;
        
        while (nanosleep(&req, &rem) == -1 && errno == EINTR) {
            req = rem;
        }
    }
};

ITime* create_time() {
    // 不使用异常处理，直接返回new的结果
    return new (std::nothrow) Time();
}

void destroy_time(ITime* time) {
    delete static_cast<Time*>(time);
}

} // namespace posix
} // namespace osal
} // namespace indurtdb

// extern "C" 工厂函数
extern "C" {
    indurtdb::osal::ITime* create_time() {
        return indurtdb::osal::posix::create_time();
    }
    
    void destroy_time(indurtdb::osal::ITime* time) {
        indurtdb::osal::posix::destroy_time(time);
    }
}