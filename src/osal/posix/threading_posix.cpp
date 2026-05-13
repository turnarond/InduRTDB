/**
 * @file threading_posix.cpp
 * @brief POSIX 线程管理实现
 * @version 2.0.0
 */

#include <indurtdb/osal/interface.hpp>
#include <pthread.h>
#include <sched.h>
#include <new>

namespace indurtdb {
namespace osal {
namespace posix {

class Threading : public IThreading {
public:
    Threading() = default;
    ~Threading() override = default;

    void set_affinity(int cpu_core) override {
        cpu_set_t cpuset;
        CPU_ZERO(&cpuset);
        CPU_SET(cpu_core, &cpuset);
        pthread_t current_thread = pthread_self();
        pthread_setaffinity_np(current_thread, sizeof(cpu_set_t), &cpuset);
    }

    void yield() override {
        sched_yield();
    }
};

IThreading* create_threading() {
    return new (std::nothrow) Threading();
}

void destroy_threading(IThreading* threading) {
    delete static_cast<Threading*>(threading);
}

} // namespace posix
} // namespace osal
} // namespace indurtdb
