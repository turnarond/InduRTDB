/**
 * @file test_seqlock.cpp
 * @brief Seqlock无锁读写算法单元测试
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 *
 * 包含Seqlock算法的完整单元测试，包括：
 * - 基础功能测试
 * - 并发访问测试
 * - 边界条件测试
 * - 性能基准测试
 */

#include "gtest/gtest.h"
#include "indurtdb/core/seqlock.hpp"
#include "indurtdb/osal/factory.hpp"
#include <thread>
#include <atomic>
#include <vector>
#include <functional>

namespace indurtdb {
namespace core {
namespace tests {

/**
 * @brief Seqlock测试基类
 *
 * 为所有Seqlock测试提供通用的设置和清理功能
 */
class SeqlockTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 创建Seqlock实例
        seqlock_ = SeqlockFactory::create();
        ASSERT_NE(seqlock_, nullptr);
        
        // 初始化时间提供器
        time_provider_ = osal::OSALFactory::create_time();
        ASSERT_NE(time_provider_, nullptr);
    }

    void TearDown() override {
        // 清理资源
        seqlock_.reset();
        time_provider_.reset();
    }

    std::unique_ptr<ISeqlock> seqlock_;          ///< Seqlock实例
    std::shared_ptr<ITime> time_provider_;     ///< 时间提供器
};

/**
 * @brief 基础功能测试
 */
TEST_F(SeqlockTest, BasicWriteReadOperation) {
    // 创建测试数据
    PointData write_data;
    write_data.timestamp_ns = time_provider_->now_ns();
    write_data.type = PointType::INT32;
    write_data.value.i = 12345;
    std::strncpy(write_data.name, "test_point", sizeof(write_data.name) - 1);
    write_data.name[sizeof(write_data.name) - 1] = '\0';

    // 测试写入操作
    EXPECT_TRUE(seqlock_->write(write_data));

    // 测试读取操作
    PointData read_data;
    EXPECT_TRUE(seqlock_->read(read_data));

    // 验证数据一致性
    EXPECT_EQ(read_data.timestamp_ns, write_data.timestamp_ns);
    EXPECT_EQ(read_data.type, write_data.type);
    EXPECT_EQ(read_data.value.i, write_data.value.i);
    EXPECT_STREQ(read_data.name, write_data.name);
}

/**
 * @brief 初始值测试
 */
TEST_F(SeqlockTest, InitialValueTest) {
    // 使用自定义初始值创建Seqlock
    PointData initial_data;
    initial_data.timestamp_ns = 1000;
    initial_data.type = PointType::DOUBLE;
    initial_data.value.d = 3.14159;

    auto seqlock = SeqlockFactory::create(initial_data);
    ASSERT_NE(seqlock, nullptr);

    PointData read_data;
    EXPECT_TRUE(seqlock->read(read_data));

    EXPECT_EQ(read_data.timestamp_ns, initial_data.timestamp_ns);
    EXPECT_EQ(read_data.type, initial_data.type);
    EXPECT_DOUBLE_EQ(read_data.value.d, initial_data.value.d);
}

/**
 * @brief 写入时读取测试
 */
TEST_F(SeqlockTest, WriteWhileReading) {
    PointData write_data;
    write_data.timestamp_ns = time_provider_->now_ns();
    write_data.type = PointType::INT32;
    write_data.value.i = 98765;

    // 在写入过程中并发读取
    bool read_successful = false;
    std::thread read_thread([&]() {
        PointData read_data;
        read_successful = seqlock_->read(read_data);
    });

    // 等待一段时间后进行写入
    std::this_thread::sleep_for(std::chrono::microseconds(10));
    EXPECT_TRUE(seqlock_->write(write_data));

    read_thread.join();
    EXPECT_TRUE(read_successful);
}

/**
 * @brief 多线程读测试
 */
TEST_F(SeqlockTest, MultiThreadedRead) {
    // 写入测试数据
    PointData write_data;
    write_data.timestamp_ns = time_provider_->now_ns();
    write_data.type = PointType::INT32;
    write_data.value.i = 54321;
    EXPECT_TRUE(seqlock_->write(write_data));

    // 创建多个读取线程
    const int thread_count = 8;
    std::vector<std::thread> threads;
    std::atomic<int> successful_reads(0);

    for (int i = 0; i < thread_count; ++i) {
        threads.emplace_back([&]() {
            PointData read_data;
            if (seqlock_->read(read_data)) {
                successful_reads++;
            }
        });
    }

    // 等待所有线程完成
    for (auto& thread : threads) {
        thread.join();
    }

    // 验证所有读取都成功
    EXPECT_EQ(successful_reads, thread_count);
}

/**
 * @brief 单写入多读取测试
 */
TEST_F(SeqlockTest, SingleWriterMultipleReaders) {
    const int reader_count = 5;
    const int write_count = 100;

    // 启动读取线程
    std::vector<std::thread> reader_threads;
    std::atomic<int> total_reads(0);

    for (int i = 0; i < reader_count; ++i) {
        reader_threads.emplace_back([&]() {
            PointData read_data;
            while (total_reads < write_count * reader_count) {
                if (seqlock_->read(read_data)) {
                    total_reads++;
                }
            }
        });
    }

    // 启动写入线程
    std::thread writer_thread([&]() {
        for (int i = 0; i < write_count; ++i) {
            PointData write_data;
            write_data.timestamp_ns = time_provider_->now_ns();
            write_data.type = PointType::INT32;
            write_data.value.i = i;
            seqlock_->write(write_data);
            std::this_thread::sleep_for(std::chrono::nanoseconds(100));
        }
    });

    // 等待所有线程完成
    writer_thread.join();
    for (auto& thread : reader_threads) {
        thread.join();
    }

    // 验证总读取次数
    EXPECT_GT(total_reads, 0);
}

/**
 * @brief 数据一致性测试
 */
TEST_F(SeqlockTest, DataConsistency) {
    // 连续写入不同类型的数据
    std::vector<PointData> test_data;

    // 测试数据1：整数
    PointData data1;
    data1.timestamp_ns = time_provider_->now_ns();
    data1.type = PointType::INT32;
    data1.value.i = 100;
    test_data.push_back(data1);

    // 测试数据2：浮点数
    PointData data2;
    data2.timestamp_ns = time_provider_->now_ns();
    data2.type = PointType::DOUBLE;
    data2.value.d = 3.14159;
    test_data.push_back(data2);

    // 测试数据3：布尔值
    PointData data3;
    data3.timestamp_ns = time_provider_->now_ns();
    data3.type = PointType::BOOL;
    data3.value.b = true;
    test_data.push_back(data3);

    // 写入所有测试数据
    for (const auto& data : test_data) {
        EXPECT_TRUE(seqlock_->write(data));
        
        PointData read_data;
        EXPECT_TRUE(seqlock_->read(read_data));
        
        EXPECT_EQ(read_data.type, data.type);
        switch (data.type) {
            case PointType::INT32:
                EXPECT_EQ(read_data.value.i, data.value.i);
                break;
            case PointType::DOUBLE:
                EXPECT_DOUBLE_EQ(read_data.value.d, data.value.d);
                break;
            case PointType::BOOL:
                EXPECT_EQ(read_data.value.b, data.value.b);
                break;
        }
    }
}

/**
 * @brief 超时行为测试
 */
TEST_F(SeqlockTest, TimeoutBehavior) {
    // 测试超时读取
    PointData read_data;
    EXPECT_THROW(seqlock_->read_with_timeout(read_data, 1), SeqlockException);
}

/**
 * @brief 序列号验证测试
 */
TEST_F(SeqlockTest, SequenceNumberValidation) {
    // 验证初始序列号为0
    EXPECT_EQ(seqlock_->get_sequence(), 0);
    EXPECT_FALSE(seqlock_->is_writing());

    // 写入操作后的序列号应该是2（0→1→2）
    PointData write_data;
    write_data.timestamp_ns = time_provider_->now_ns();
    write_data.type = PointType::INT32;
    write_data.value.i = 123;
    EXPECT_TRUE(seqlock_->write(write_data));

    EXPECT_EQ(seqlock_->get_sequence(), 2);
    EXPECT_FALSE(seqlock_->is_writing());
}

/**
 * @brief 性能基准测试
 */
TEST_F(SeqlockTest, PerformanceBenchmark) {
    const int iterations = 100000;

    // 写入性能测试
    PointData write_data;
    write_data.timestamp_ns = time_provider_->now_ns();
    write_data.type = PointType::INT32;
    write_data.value.i = 0;

    uint64_t write_start = time_provider_->now_ns();
    for (int i = 0; i < iterations; ++i) {
        write_data.value.i = i;
        seqlock_->write(write_data);
    }
    uint64_t write_end = time_provider_->now_ns();

    // 读取性能测试
    PointData read_data;
    uint64_t read_start = time_provider_->now_ns();
    for (int i = 0; i < iterations; ++i) {
        seqlock_->read(read_data);
    }
    uint64_t read_end = time_provider_->now_ns();

    // 计算平均时间
    double avg_write_time = static_cast<double>(write_end - write_start) / iterations;
    double avg_read_time = static_cast<double>(read_end - read_start) / iterations;

    // 输出性能指标
    std::cout << "Write performance: " << avg_write_time << " ns/operation" << std::endl;
    std::cout << "Read performance: " << avg_read_time << " ns/operation" << std::endl;

    // 验证性能指标（这些值根据实际硬件可能需要调整）
    EXPECT_LT(avg_write_time, 100);  // 写入操作应小于100ns
    EXPECT_LT(avg_read_time, 10);   // 读取操作应小于10ns
}

/**
 * @brief 重置功能测试
 */
TEST_F(SeqlockTest, ResetFunctionality) {
    PointData initial_data;
    initial_data.timestamp_ns = 1000;
    initial_data.type = PointType::INT32;
    initial_data.value.i = 123;

    // 重置Seqlock
    seqlock_->reset(initial_data);

    PointData read_data;
    EXPECT_TRUE(seqlock_->read(read_data));
    EXPECT_EQ(read_data.value.i, initial_data.value.i);
}

/**
 * @brief 边界条件测试
 */
TEST_F(SeqlockTest, BoundaryConditions) {
    // 测试最大值和最小值
    PointData min_data;
    min_data.timestamp_ns = 0;
    min_data.type = PointType::INT32;
    min_data.value.i = INT32_MIN;
    EXPECT_TRUE(seqlock_->write(min_data));

    PointData max_data;
    max_data.timestamp_ns = time_provider_->now_ns();
    max_data.type = PointType::INT32;
    max_data.value.i = INT32_MAX;
    EXPECT_TRUE(seqlock_->write(max_data));

    PointData read_data;
    EXPECT_TRUE(seqlock_->read(read_data));
    EXPECT_EQ(read_data.value.i, INT32_MAX);
}

} // namespace tests
} // namespace core
} // namespace indurtdb

int main(int argc, char **argv) {
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
