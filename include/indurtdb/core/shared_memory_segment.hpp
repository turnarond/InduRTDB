/**
 * @file shared_memory_segment.hpp
 * @brief SharedMemorySegment 公共接口
 * @version 2.1.0
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include <memory>
#include <cstddef>

namespace indurtdb {
namespace osal { class ISharedMemory; }

namespace core {

class SharedMemorySegment {
public:
    SharedMemorySegment(const char* instance_id,
                        uint32_t max_points,
                        uint32_t max_subscribers);
    ~SharedMemorySegment();

    bool initialize();
    void shutdown();

    void*            base() const;
    InduRTDBHeader*  header() const;
    PointData*       points() const;
    SubscriberEntry* subscribers() const;

    uint32_t max_points() const;
    uint32_t max_subscribers() const;
    size_t   total_size() const;
    bool     is_owner() const;

private:
    char   name_[64];
    void*  base_;
    size_t total_size_;
    bool   is_owner_;

    InduRTDBHeader*  header_;
    PointData*       points_;
    SubscriberEntry* subscribers_;

    uint32_t max_points_;
    uint32_t max_subscribers_;

    // 保持 OSAL 共享内存对象存活，否则 munmap 会释放映射
    std::unique_ptr<osal::ISharedMemory> shm_;

    void init_header();
    bool validate_header() const;
};

} // namespace core
} // namespace indurtdb
