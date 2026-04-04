/**
 * @file indurtdb.hpp
 * @brief C++ API接口定义
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#pragma once

#include "../types/basic_types.hpp"
#include "../types/memory_layout.hpp"
#include <memory>
#include <unordered_map>
#include <mutex>
#include <functional>
#include <string>
#include <vector>

namespace indurtdb {

class InduRTDB {
public:
    class Impl;
    
    static InduRTDB& instance();
    
    bool initialize(const std::string& instance_id,
                   std::size_t max_points,
                   std::size_t max_subscribers);
    
    bool write(PointId id, bool value);
    bool write(PointId id, int32_t value);
    bool write(PointId id, double value);
    bool write(PointId id, const char* value);
    
    bool read(PointId id, PointData& out) const;
    const PointData* peek(PointId id) const;
    
    bool subscribe(PointId id, std::function<void(const PointData&)> callback);
    bool unsubscribe(PointId id);
    
    bool load_config(const std::string& config_path);
    
    void update_heartbeat();
    bool is_initialized() const;
    
    void shutdown();
    
private:
    InduRTDB();
    ~InduRTDB();
    
    InduRTDB(const InduRTDB&) = delete;
    InduRTDB& operator=(const InduRTDB&) = delete;
    
    Impl* impl_;
};

} // namespace indurtdb