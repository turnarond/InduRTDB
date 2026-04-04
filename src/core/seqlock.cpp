/**
 * @file seqlock.cpp
 * @brief Seqlock无锁读写算法实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 *
 * 实现Seqlock算法的核心功能，包括：
 * - 读操作和写操作的核心算法
 * - 线程安全的并发控制机制
 * - 异常处理和超时机制
 * - 性能优化策略
 */

#include <indurtdb/core/seqlock.hpp>
#include <indurtdb/osal/factory.hpp>
#include <cstring>
#include <atomic>

namespace indurtdb {
namespace core {

// 内存屏障工具函数
void Seqlock::memory_fence_acquire() {
    __atomic_thread_fence(__ATOMIC_ACQUIRE);
}

void Seqlock::memory_fence_release() {
    __atomic_thread_fence(__ATOMIC_RELEASE);
}

void Seqlock::memory_fence_full() {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

// 构造函数
Seqlock::Seqlock(const PointData& initial_value) {
    // 初始化序列号为0（偶数，表示稳定状态）
    __atomic_store_n(&lock_data_.sequence, 0, __ATOMIC_RELEASE);
    
    // 初始化点位数据
    std::memcpy(&lock_data_.point_data, &initial_value, sizeof(PointData));
    
    // 确保数据可见性
    memory_fence_full();
}

// 写入操作
bool Seqlock::write(const PointData& data) {
    // 获取当前序列号
    SeqlockSequence seq = __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
    
    // 检查是否有其他写入正在进行
    if (seq % 2 != 0) {
        return false; // 有写入正在进行，写入失败
    }
    
    // 增加序列号（奇数，表示正在写入）
    __atomic_store_n(&lock_data_.sequence, seq + 1, __ATOMIC_RELEASE);
    
    // 内存屏障确保操作顺序
    memory_fence_full();
    
    // 实际写入数据
    std::memcpy(&lock_data_.point_data, &data, sizeof(PointData));
    
    // 内存屏障确保数据可见性
    memory_fence_full();
    
    // 再次增加序列号（偶数，表示写入完成）
    __atomic_store_n(&lock_data_.sequence, seq + 2, __ATOMIC_RELEASE);
    
    return true;
}

// 读取操作
bool Seqlock::read(PointData& data) const {
    SeqlockSequence seq1, seq2;
    
    do {
        // 读取序列号（获取内存屏障）
        seq1 = __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
        
        // 检查是否正在写入
        if (seq1 % 2 != 0) {
            continue; // 正在写入，重试
        }
        
        // 内存屏障确保数据完整性
        memory_fence_acquire();
        
        // 读取数据
        std::memcpy(&data, &lock_data_.point_data, sizeof(PointData));
        
        // 内存屏障确保读取顺序
        memory_fence_release();
        
        // 再次读取序列号
        seq2 = __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
        
    } while (seq1 != seq2); // 如果序列号不同，重试
    
    return true;
}

// 带超时的读取操作
bool Seqlock::read_with_timeout(PointData& data, uint64_t timeout_ns) const {
    auto time_provider = osal::OSALFactory::create_time();
    uint64_t start = time_provider->now_ns();
    
    while ((time_provider->now_ns() - start) < timeout_ns) {
        if (read(data)) {
            return true;
        }
    }
    
    // 超时返回false，不抛出异常
    return false;
}

// 获取当前序列号
SeqlockSequence Seqlock::get_sequence() const {
    return __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
}

// 检查是否正在写入
bool Seqlock::is_writing() const {
    SeqlockSequence seq = __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
    return (seq % 2 != 0);
}

// 重置Seqlock
void Seqlock::reset(const PointData& initial_value) {
    // 获取当前序列号
    SeqlockSequence seq = __atomic_load_n(&lock_data_.sequence, __ATOMIC_ACQUIRE);
    
    // 确保写入状态
    if (seq % 2 == 0) {
        __atomic_store_n(&lock_data_.sequence, seq + 1, __ATOMIC_RELEASE);
    }
    
    // 内存屏障
    memory_fence_full();
    
    // 重置数据
    std::memcpy(&lock_data_.point_data, &initial_value, sizeof(PointData));
    
    // 内存屏障
    memory_fence_full();
    
    // 设置为稳定状态
    __atomic_store_n(&lock_data_.sequence, seq + 2, __ATOMIC_RELEASE);
}

// Seqlock工厂实现
std::unique_ptr<ISeqlock> SeqlockFactory::create(const PointData& initial_value) {
    return std::make_unique<Seqlock>(initial_value);
}

std::shared_ptr<ISeqlock> SeqlockFactory::create_shared(const PointData& initial_value) {
    return std::make_shared<Seqlock>(initial_value);
}

// SeqlockUtils实现
uint64_t SeqlockUtils::measure_time(const std::function<void()>& operation) {
    auto time_provider = osal::OSALFactory::create_time();
    uint64_t start = time_provider->now_ns();
    
    operation();
    
    return time_provider->now_ns() - start;
}

double SeqlockUtils::benchmark_read_performance(ISeqlock& seqlock, uint64_t iterations) {
    PointData data;
    
    auto total_time = measure_time([&]() {
        for (uint64_t i = 0; i < iterations; ++i) {
            seqlock.read(data);
        }
    });
    
    return static_cast<double>(total_time) / static_cast<double>(iterations);
}

double SeqlockUtils::benchmark_write_performance(ISeqlock& seqlock, uint64_t iterations) {
    PointData data;
    data.timestamp_ns = osal::OSALFactory::create_time()->now_ns();
    data.type = PointType::INT32;
    data.value.i = 0;
    
    auto total_time = measure_time([&]() {
        for (uint64_t i = 0; i < iterations; ++i) {
            data.value.i = static_cast<int32_t>(i);
            seqlock.write(data);
        }
    });
    
    return static_cast<double>(total_time) / static_cast<double>(iterations);
}

} // namespace core
} // namespace indurtdb
