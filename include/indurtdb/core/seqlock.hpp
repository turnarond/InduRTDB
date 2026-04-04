/**
 * @file seqlock.hpp
 * @brief Seqlock无锁读写算法实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 *
 * Seqlock是一种基于序列号的无锁读写算法，适用于读多写少的场景
 * 其核心思想是通过序列号来保证数据一致性，读操作无锁，写操作通过
 * 序列号的奇偶性来标记写入状态
 *
 * 适用场景：
 * - 工业实时数据库系统
 * - 高并发读多写少的场景
 * - 需要低延迟访问的实时系统
 */

#pragma once

#include "point_manager_interface.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <stdexcept>

namespace indurtdb {
namespace core {

// 基础序列号类型
using SeqlockSequence = uint64_t;

/**
 * @brief Seqlock异常类
 *
 * 用于表示Seqlock操作过程中的错误
 */
class SeqlockException : public std::runtime_error {
public:
    enum class ErrorType {
        WRITE_FAILED,        ///< 写操作失败
        READ_TIMEOUT,        ///< 读操作超时
        DATA_CORRUPT,        ///< 数据损坏
        INVALID_OPERATION    ///< 无效操作
    };

    /**
     * @brief 构造函数
     * @param error 错误类型
     * @param message 错误信息
     */
    SeqlockException(ErrorType error, const std::string& message)
        : std::runtime_error(message), error_type_(error) {}

    /**
     * @brief 获取错误类型
     * @return 错误类型枚举值
     */
    ErrorType get_error_type() const { return error_type_; }

private:
    ErrorType error_type_;
};

/**
 * @brief Seqlock数据结构
 *
 * 包含序列号和实际的点位数据
 */
struct alignas(64) SeqlockData {
    volatile SeqlockSequence sequence;    ///< 序列号（偶数表示稳定，奇数表示正在写入）
    PointData point_data;                ///< 点位数据
};

/**
 * @brief Seqlock接口
 *
 * 定义Seqlock的基本操作接口
 */
class ISeqlock {
public:
    virtual ~ISeqlock() = default;

    /**
     * @brief 写入数据
     * @param data 要写入的点位数据
     * @return 是否成功写入
     */
    virtual bool write(const PointData& data) = 0;

    /**
     * @brief 读取数据
     * @param data 读取到的数据将存储在此处
     * @return 是否成功读取到一致的数据
     */
    virtual bool read(PointData& data) const = 0;

    /**
     * @brief 带超时的读取操作
     * @param data 读取到的数据将存储在此处
     * @param timeout_ns 超时时间（纳秒）
     * @return 是否成功读取到一致的数据
     * @throws SeqlockException 读取超时
     */
    virtual bool read_with_timeout(PointData& data, uint64_t timeout_ns) const = 0;

    /**
     * @brief 获取当前序列号
     * @return 当前序列号
     */
    virtual SeqlockSequence get_sequence() const = 0;

    /**
     * @brief 检查是否正在写入
     * @return 是否正在写入
     */
    virtual bool is_writing() const = 0;

    /**
     * @brief 重置Seqlock
     * @param initial_value 初始值
     */
    virtual void reset(const PointData& initial_value = PointData()) = 0;
};

/**
 * @brief Seqlock实现类
 *
 * 线程安全的Seqlock实现，支持无锁读操作和无锁写操作
 */
class Seqlock : public ISeqlock {
public:
    /**
     * @brief 构造函数
     * @param initial_value 初始值
     */
    explicit Seqlock(const PointData& initial_value = PointData());

    /**
     * @brief 写入数据（无锁操作）
     * @param data 要写入的点位数据
     * @return 是否成功写入
     */
    bool write(const PointData& data) override;

    /**
     * @brief 读取数据（无锁操作）
     * @param data 读取到的数据将存储在此处
     * @return 是否成功读取到一致的数据
     */
    bool read(PointData& data) const override;

    /**
     * @brief 带超时的读取操作
     * @param data 读取到的数据将存储在此处
     * @param timeout_ns 超时时间（纳秒）
     * @return 是否成功读取到一致的数据
     * @throws SeqlockException 读取超时
     */
    bool read_with_timeout(PointData& data, uint64_t timeout_ns) const override;

    /**
     * @brief 获取当前序列号
     * @return 当前序列号
     */
    SeqlockSequence get_sequence() const override;

    /**
     * @brief 检查是否正在写入
     * @return 是否正在写入
     */
    bool is_writing() const override;

    /**
     * @brief 重置Seqlock
     * @param initial_value 初始值
     */
    void reset(const PointData& initial_value = PointData()) override;

    /**
     * @brief 获取数据引用（仅用于调试和性能分析）
     * @return 数据引用（非线程安全）
     */
    const PointData& debug_get_data() const { return lock_data_.point_data; }

private:
    /**
     * @brief 内存屏障工具函数
     */
    static void memory_fence_acquire();
    static void memory_fence_release();
    static void memory_fence_full();

    mutable SeqlockData lock_data_;    ///< 底层数据结构（mutable允许const方法修改）
};

/**
 * @brief Seqlock工厂类
 *
 * 提供Seqlock实例的创建方法
 */
class SeqlockFactory {
public:
    /**
     * @brief 创建Seqlock实例
     * @param initial_value 初始值
     * @return Seqlock智能指针
     */
    static std::unique_ptr<ISeqlock> create(const PointData& initial_value = PointData());

    /**
     * @brief 创建Seqlock实例（线程安全版本）
     * @param initial_value 初始值
     * @return Seqlock智能指针
     */
    static std::shared_ptr<ISeqlock> create_shared(const PointData& initial_value = PointData());
};

/**
 * @brief Seqlock实用工具函数
 */
class SeqlockUtils {
public:
    /**
     * @brief 计算操作耗时
     * @param operation 要测量的操作
     * @return 操作耗时（纳秒）
     */
    static uint64_t measure_time(const std::function<void()>& operation);

    /**
     * @brief 性能基准测试
     * @param seqlock Seqlock实例
     * @param iterations 迭代次数
     * @return 平均操作时间（纳秒）
     */
    static double benchmark_read_performance(ISeqlock& seqlock, uint64_t iterations);

    /**
     * @brief 性能基准测试
     * @param seqlock Seqlock实例
     * @param iterations 迭代次数
     * @return 平均操作时间（纳秒）
     */
    static double benchmark_write_performance(ISeqlock& seqlock, uint64_t iterations);
};

} // namespace core
} // namespace indurtdb
