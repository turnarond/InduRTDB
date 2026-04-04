/**
 * @file threading_base.cpp
 * @brief 线程接口基类实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include "../include/indurtdb/osal/interface.hpp"

namespace indurtdb {
namespace osal {

class ThreadingBase : public IThreading {
public:
    ThreadingBase() = default;
    
    virtual ~ThreadingBase() = default;
    
    virtual void set_affinity(int cpu_core) {
        (void)cpu_core;
    }
    
    virtual void yield() {
        // 待实现
    }
};

} // namespace osal
} // namespace indurtdb
