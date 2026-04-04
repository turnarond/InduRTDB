/**
 * @file point_manager_interface.hpp
 * @brief 点位管理器接口定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include <cstdint>
#include <cstring>
#include <memory>

namespace indurtdb {
namespace core {

class PointManager {
public:
    virtual ~PointManager() = default;
    
    virtual bool write_bool(PointId id, bool value) = 0;
    virtual bool write_int32(PointId id, int32_t value) = 0;
    virtual bool write_double(PointId id, double value) = 0;
    virtual bool write_string(PointId id, const char* value) = 0;
    
    virtual bool read(PointId id, PointData& out) const = 0;
    virtual const PointData* peek(PointId id) const = 0;
    
    virtual bool validate_id(PointId id) const = 0;
    
    virtual uint64_t get_write_count() const = 0;
    virtual uint64_t get_timeout_count() const = 0;
    
protected:
    PointManager() = default;
    
    PointManager(const PointManager&) = delete;
    PointManager& operator=(const PointManager&) = delete;
};

PointManager* create_point_manager(const std::string& instance_id,
                                  std::size_t max_points,
                                  std::size_t max_subscribers);

void destroy_point_manager(PointManager* manager);

} // namespace core
} // namespace indurtdb