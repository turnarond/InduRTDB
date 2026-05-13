/**
 * @file point_manager_interface.hpp
 * @brief PointManager —— 直接操作共享内存（非虚类，编译期绑定）
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 *
 * PointManager 不持有共享内存所有权，由 SharedMemorySegment 传入 mmap 基址。
 * 使用全局 Seqlock (header_->write_seq) 保护并发写入。
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include "../osal/interface.hpp"
#include "seqlock.hpp"
#include <cstring>
#include <type_traits>

namespace indurtdb {
namespace core {

class PointManager {
public:
    /**
     * @brief 构造函数
     * @param shm_base   共享内存 mmap 基址
     * @param max_points 最大点位数
     * @param time       OSAL 时间接口
     */
    PointManager(void* shm_base, uint32_t max_points, osal::ITime* time)
        : header_(static_cast<InduRTDBHeader*>(shm_base))
        , points_(reinterpret_cast<PointData*>(
              static_cast<char*>(shm_base) + sizeof(InduRTDBHeader)))
        , max_points_(max_points)
        , time_(time)
    {}

    // ---- 模板写入（满足 SRS: rtdb.write(id, value)） ----

    template<typename T>
    bool write(PointId id, const T& value) {
        if (!validate_id(id)) return false;

        uint64_t seq0 = seqlock_write_begin(&header_->write_seq);
        if (seq0 & 1ULL) return false;  // 写冲突

        PointData* p = &points_[id];
        if constexpr (std::is_same_v<T, bool>) {
            p->value.b = value; p->type = PointType::BOOL;
        } else if constexpr (std::is_same_v<T, int32_t>) {
            p->value.i = value; p->type = PointType::INT32;
        } else if constexpr (std::is_same_v<T, double>) {
            p->value.d = value; p->type = PointType::DOUBLE;
        } else if constexpr (std::is_same_v<T, const char*>) {
            std::strncpy(p->value.str, value, sizeof(p->value.str) - 1);
            p->value.str[sizeof(p->value.str) - 1] = '\0';
            p->type = PointType::STRING;
        } else {
            // 不支持的类型 —— 编译期就会失败
            static_assert(sizeof(T) == 0, "Unsupported write type");
        }
        p->timestamp_ns = time_->now_ns();
        p->quality = Quality::GOOD;

        seqlock_write_end(&header_->write_seq, seq0);
        __atomic_fetch_add(&header_->stats.writes, 1ULL, __ATOMIC_RELAXED);
        return true;
    }

    // ---- 读取（带拷贝） ----

    bool read(PointId id, PointData& out) const {
        if (!validate_id(id)) return false;
        const PointData* p = seqlock_read(&header_->write_seq, points_, id);
        if (!p) return false;
        std::memcpy(&out, p, sizeof(PointData));
        return true;
    }

    // ---- 零拷贝 peek（返回共享内存指针） ----

    const PointData* peek(PointId id) const {
        if (!validate_id(id)) return nullptr;
        return seqlock_read(&header_->write_seq, points_, id);
    }

    // ---- 辅助方法 ----

    bool validate_id(PointId id) const {
        return id < max_points_;
    }

    uint64_t get_write_count() const {
        return __atomic_load_n(&header_->stats.writes, __ATOMIC_RELAXED);
    }

    uint64_t get_timeout_count() const {
        return __atomic_load_n(&header_->stats.timeouts, __ATOMIC_RELAXED);
    }

    uint32_t max_points() const { return max_points_; }

    // 获取 Header 指针（供 SubscriptionManager 访问心跳表）
    InduRTDBHeader* header() const { return header_; }
    PointData*      points() const { return points_; }

private:
    InduRTDBHeader* header_;
    PointData*      points_;
    uint32_t        max_points_;
    osal::ITime*    time_;
};

} // namespace core
} // namespace indurtdb
