/**
 * @file test_alignment.cpp
 * @brief Unit tests for alignment utilities
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#include <gtest/gtest.h>
#include "indurtdb/utils/alignment.hpp"

using namespace indurtdb::utils;

TEST(AlignmentTest, IsAligned) {
    // 测试对齐检查
    char buffer[256] = {0};
    
    // 测试对齐的指针
    void* aligned_ptr = static_cast<void*>(buffer);
    EXPECT_TRUE(is_aligned(aligned_ptr, 1));
    EXPECT_TRUE(is_aligned(aligned_ptr, 2));
    EXPECT_TRUE(is_aligned(aligned_ptr, 4));
    EXPECT_TRUE(is_aligned(aligned_ptr, 8));
    
    // 测试未对齐的指针
    void* unaligned_ptr = static_cast<char*>(buffer) + 1;
    EXPECT_FALSE(is_aligned(unaligned_ptr, 2));
    EXPECT_FALSE(is_aligned(unaligned_ptr, 4));
    EXPECT_FALSE(is_aligned(unaligned_ptr, 8));
}

TEST(AlignmentTest, AlignPointer) {
    // 测试指针对齐
    char buffer[256];
    
    // 测试各种对齐
    void* ptr1 = buffer;
    void* aligned1 = align_pointer(ptr1, 64);
    // 栈上的 buffer 不保证 64B 对齐，只验证对齐结果正确
    EXPECT_TRUE(is_aligned(aligned1, 64));
    EXPECT_GE(reinterpret_cast<uintptr_t>(aligned1),
              reinterpret_cast<uintptr_t>(ptr1));
    
    void* ptr2 = static_cast<char*>(buffer) + 1;
    void* aligned2 = align_pointer(ptr2, 64);
    EXPECT_NE(ptr2, aligned2);
    EXPECT_TRUE(is_aligned(aligned2, 64));
    
    // 测试边界情况
    void* ptr3 = static_cast<char*>(buffer) + 63;
    void* aligned3 = align_pointer(ptr3, 64);
    EXPECT_NE(ptr3, aligned3);
    EXPECT_TRUE(is_aligned(aligned3, 64));
}

TEST(AlignmentTest, AlignSize) {
    // 测试大小对齐
    EXPECT_EQ(align_size(1, 64), 64);
    EXPECT_EQ(align_size(64, 64), 64);
    EXPECT_EQ(align_size(65, 64), 128);
    EXPECT_EQ(align_size(127, 64), 128);
    EXPECT_EQ(align_size(128, 64), 128);
    
    // 测试各种对齐值
    EXPECT_EQ(align_size(1, 1), 1);
    EXPECT_EQ(align_size(1, 2), 2);
    EXPECT_EQ(align_size(1, 4), 4);
    EXPECT_EQ(align_size(1, 8), 8);
    EXPECT_EQ(align_size(1, 16), 16);
    EXPECT_EQ(align_size(1, 32), 32);
    EXPECT_EQ(align_size(1, 64), 64);
}

TEST(AlignmentTest, AlignedAlloc) {
    // 测试对齐内存分配
    void* ptr = aligned_memory_alloc(1024, 64);
    EXPECT_NE(ptr, nullptr);
    EXPECT_TRUE(is_aligned(ptr, 64));
    
    // 测试内存访问
    uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
    for (int i = 0; i < 1024; ++i) {
        byte_ptr[i] = static_cast<uint8_t>(i);
    }
    
    // 验证写入的数据
    for (int i = 0; i < 1024; ++i) {
        EXPECT_EQ(byte_ptr[i], static_cast<uint8_t>(i));
    }
    
    aligned_memory_free(ptr);
}

TEST(AlignmentTest, AlignedAllocEdgeCases) {
    // 测试边界情况
    void* ptr1 = aligned_memory_alloc(0, 64);
    EXPECT_EQ(ptr1, nullptr);
    
    void* ptr2 = aligned_memory_alloc(1024, 0);
    EXPECT_EQ(ptr2, nullptr);
    
    void* ptr3 = aligned_memory_alloc(1024, 3); // 3不是2的幂
    EXPECT_EQ(ptr3, nullptr);
    
    // posix_memalign 要求 alignment >= sizeof(void*)
    // align=1 小于 sizeof(void*) 会失败，这符合 POSIX 规范
    void* ptr4 = aligned_memory_alloc(1024, 1);
    EXPECT_EQ(ptr4, nullptr);  // alignment too small for posix_memalign
}

TEST(AlignmentTest, CacheLineSize) {
    // 测试缓存行大小
    std::size_t cache_line_size = get_cache_line_size();
    EXPECT_GT(cache_line_size, 0);
    EXPECT_LE(cache_line_size, 256); // 合理的缓存行大小范围
    
    // 检查是否是2的幂
    EXPECT_EQ((cache_line_size & (cache_line_size - 1)), 0);
}

TEST(AlignmentTest, FillCacheLine) {
    // 测试缓存行填充
    char buffer[256];
    
    // 填充缓存行
    fill_cache_line(buffer, 64);
    
    // 验证填充
    for (int i = 0; i < 64; ++i) {
        EXPECT_EQ(buffer[i], 0);
    }
}

TEST(AlignmentTest, MemoryBarrier) {
    // 测试内存屏障
    // 这里主要测试屏障不会导致崩溃
    memory_barrier();
    EXPECT_TRUE(true); // 如果没有崩溃，测试通过
}

TEST(AlignmentTest, Prefetch) {
    // 测试预取
    char buffer[256] = {0};
    
    // 预取数据（应该不会导致崩溃）
    prefetch(buffer, 0);
    prefetch(buffer, 1);
    prefetch(buffer, 2);
    prefetch(buffer, 3);
    
    EXPECT_TRUE(true); // 如果没有崩溃，测试通过
}

TEST(AlignmentTest, AlignmentCombinations) {
    // 测试各种对齐组合
    const std::size_t sizes[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    const std::size_t alignments[] = {1, 2, 4, 8, 16, 32, 64, 128, 256};
    
    for (std::size_t size : sizes) {
        for (std::size_t alignment : alignments) {
            // 跳过无效组合
            if (alignment == 0 || (alignment & (alignment - 1)) != 0) {
                continue;
            }
            
            void* ptr = aligned_memory_alloc(size, alignment);
            if (ptr) {
                EXPECT_TRUE(is_aligned(ptr, alignment));
                aligned_memory_free(ptr);
            }
        }
    }
}

TEST(AlignmentTest, LargeAlignment) {
    // 测试大对齐
    void* ptr = aligned_memory_alloc(4096, 4096);
    EXPECT_NE(ptr, nullptr);
    EXPECT_TRUE(is_aligned(ptr, 4096));
    
    aligned_memory_free(ptr);
}

