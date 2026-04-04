/**
 * @file alignment.hpp
 * @brief Alignment utilities for memory operations
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include <cstddef>
#include <cstdlib>
#include <cstring>

namespace indurtdb {
namespace utils {

/**
 * @brief Check if a pointer is aligned to a specific boundary
 * 
 * @param ptr Pointer to check
 * @param alignment Alignment boundary (must be power of 2)
 * @return true Pointer is aligned
 * @return false Pointer is not aligned
 */
inline bool is_aligned(const void* ptr, std::size_t alignment) {
    return (reinterpret_cast<std::uintptr_t>(ptr) & (alignment - 1)) == 0;
}

/**
 * @brief Align a pointer to a specific boundary
 * 
 * @param ptr Pointer to align
 * @param alignment Alignment boundary (must be power of 2)
 * @return void* Aligned pointer
 */
inline void* align_pointer(void* ptr, std::size_t alignment) {
    std::uintptr_t addr = reinterpret_cast<std::uintptr_t>(ptr);
    std::uintptr_t aligned_addr = (addr + alignment - 1) & ~(alignment - 1);
    return reinterpret_cast<void*>(aligned_addr);
}

/**
 * @brief Align a size to a specific boundary
 * 
 * @param size Size to align
 * @param alignment Alignment boundary (must be power of 2)
 * @return std::size_t Aligned size
 */
inline std::size_t align_size(std::size_t size, std::size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

/**
 * @brief Allocate aligned memory
 * 
 * @param size Size to allocate
 * @param alignment Alignment boundary (must be power of 2)
 * @return void* Aligned pointer, or nullptr on failure
 */
inline void* aligned_memory_alloc(std::size_t size, std::size_t alignment) {
    if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0) {
        return nullptr;
    }
    
    void* ptr = nullptr;
    int result = posix_memalign(&ptr, alignment, size);
    if (result != 0) {
        return nullptr;
    }
    
    return ptr;
}

/**
 * @brief Free aligned memory
 * 
 * @param ptr Pointer to free
 */
inline void aligned_memory_free(void* ptr) {
    free(ptr);
}

/**
 * @brief Get cache line size
 * 
 * @return std::size_t Cache line size
 */
inline std::size_t get_cache_line_size() {
    // Default cache line size for x86-64
    return 64;
}

/**
 * @brief Fill a cache line with zeros
 * 
 * @param ptr Pointer to fill
 * @param size Size to fill
 */
inline void fill_cache_line(void* ptr, std::size_t size) {
    std::memset(ptr, 0, size);
}

/**
 * @brief Memory barrier
 */
inline void memory_barrier() {
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
}

/**
 * @brief Prefetch data into cache
 * 
 * @param ptr Pointer to prefetch
 * @param locality Locality hint (0-3)
 */
inline void prefetch(const void* ptr, int locality) {
    (void)ptr;
    (void)locality;
    // No-op implementation
}

} // namespace utils
} // namespace indurtdb