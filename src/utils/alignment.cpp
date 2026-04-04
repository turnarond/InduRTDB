/**
 * @file alignment.cpp
 * @brief Memory alignment utilities
 * @version 1.0.0
 * @date 2026-03-27
 * 
 * @copyright MIT License
 */

#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace indurtdb {
namespace utils {

// 检查是否是对齐的
bool is_aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<uintptr_t>(ptr) & (alignment - 1)) == 0;
}

// 对齐指针
void* align_pointer(void* ptr, std::size_t alignment) {
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
    return reinterpret_cast<void*>(aligned_addr);
}

// 对齐大小
std::size_t align_size(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

// 分配对齐内存
void* aligned_alloc(std::size_t size, std::size_t alignment) {
    // 检查对齐是否是2的幂
    if ((alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    
    // 分配额外空间用于存储原始指针
    std::size_t total_size = size + alignment + sizeof(void*);
    void* raw_ptr = std::malloc(total_size);
    
    if (!raw_ptr) {
        return nullptr;
    }
    
    // 对齐指针
    void* aligned_ptr = align_pointer(
        static_cast<char*>(raw_ptr) + sizeof(void*),
        alignment
    );
    
    // 在对齐指针之前存储原始指针
    void** storage = static_cast<void**>(aligned_ptr) - 1;
    *storage = raw_ptr;
    
    return aligned_ptr;
}

// 释放对齐内存
void aligned_free(void* ptr) {
    if (!ptr) {
        return;
    }
    
    // 获取原始指针
    void** storage = static_cast<void**>(ptr) - 1;
    void* raw_ptr = *storage;
    
    std::free(raw_ptr);
}

// 检查缓存行大小
std::size_t get_cache_line_size() {
    // 默认缓存行大小（通常为64字节）
    constexpr std::size_t DEFAULT_CACHE_LINE_SIZE = 64;
    
    // 这里可以添加平台特定的代码来获取实际缓存行大小
    // 例如，在x86上可以使用cpuid指令
    // 在ARM上可以使用CTR_EL0寄存器
    
    return DEFAULT_CACHE_LINE_SIZE;
}

// 填充缓存行
void fill_cache_line(void* ptr, std::size_t size) {
    if (!ptr || size == 0) {
        return;
    }
    
    // 使用volatile防止编译器优化
    volatile uint8_t* byte_ptr = static_cast<uint8_t*>(ptr);
    for (std::size_t i = 0; i < size; ++i) {
        byte_ptr[i] = 0;
    }
}

// 内存屏障
void memory_barrier() {
    // 编译器屏障
    asm volatile("" ::: "memory");
    
    // 这里可以添加平台特定的内存屏障指令
    // 例如，在x86上：asm volatile("mfence" ::: "memory");
    // 在ARM上：asm volatile("dmb sy" ::: "memory");
}

// 预取数据
void prefetch(const void* ptr, int locality) {
    // 这里可以添加平台特定的预取指令
    // 例如，在x86上：__builtin_prefetch(ptr, 0, locality);
    // locality: 0 - 无时间局部性，1 - 低时间局部性，2 - 中等时间局部性，3 - 高时间局部性
    
    (void)ptr;
    (void)locality;
}

} // namespace utils
} // namespace indurtdb