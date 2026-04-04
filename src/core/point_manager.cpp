/**
 * @file point_manager.cpp
 * @brief PointManager实现（使用Seqlock算法）
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <indurtdb/core/point_manager_interface.hpp>
#include <indurtdb/core/seqlock.hpp>
#include <indurtdb/osal/factory.hpp>

namespace indurtdb {
namespace core {

class PointManagerImpl : public PointManager {
public:
    PointManagerImpl(const std::string& instance_id,
                    size_t max_points,
                    size_t max_subscribers)
        : instance_id_(instance_id),
          max_points_(max_points),
          max_subscribers_(max_subscribers),
          segment_(nullptr),
          time_(nullptr) {
        
        time_ = osal::OSALFactory::create_time();
        point_locks_.reserve(max_points);
        for (size_t i = 0; i < max_points; ++i) {
            point_locks_.emplace_back(SeqlockFactory::create());
        }
    }
    
    ~PointManagerImpl() override = default;
    
    bool write_bool(uint32_t id, bool value) override {
        if (!validate_id(id)) {
            return false;
        }
        
        PointData data;
        data.timestamp_ns = time_->now_ns();
        data.type = PointType::BOOL;
        data.value.b = value;
        data.quality = Quality::GOOD;
        data.unit = Unit::NO_UNIT;
        data.access = Access::READ_WRITE;
        
        return point_locks_[id]->write(data);
    }
    
    bool write_int32(uint32_t id, int32_t value) override {
        if (!validate_id(id)) {
            return false;
        }
        
        PointData data;
        data.timestamp_ns = time_->now_ns();
        data.type = PointType::INT32;
        data.value.i = value;
        data.quality = Quality::GOOD;
        data.unit = Unit::NO_UNIT;
        data.access = Access::READ_WRITE;
        
        return point_locks_[id]->write(data);
    }
    
    bool write_double(uint32_t id, double value) override {
        if (!validate_id(id)) {
            return false;
        }
        
        PointData data;
        data.timestamp_ns = time_->now_ns();
        data.type = PointType::DOUBLE;
        data.value.d = value;
        data.quality = Quality::GOOD;
        data.unit = Unit::NO_UNIT;
        data.access = Access::READ_WRITE;
        
        return point_locks_[id]->write(data);
    }
    
    bool write_string(uint32_t id, const char* value) override {
        if (!validate_id(id) || !value) {
            return false;
        }
        
        std::size_t len = std::strlen(value);
        if (len >= 32) {
            return false;
        }
        
        PointData data;
        data.timestamp_ns = time_->now_ns();
        data.type = PointType::STRING;
        std::strncpy(data.value.str, value, sizeof(data.value.str) - 1);
        data.value.str[sizeof(data.value.str) - 1] = '\0';
        data.quality = Quality::GOOD;
        data.unit = Unit::NO_UNIT;
        data.access = Access::READ_WRITE;
        
        return point_locks_[id]->write(data);
    }
    
    bool read(uint32_t id, PointData& out) const override {
        if (!validate_id(id)) {
            return false;
        }
        
        return point_locks_[id]->read(out);
    }
    
    const PointData* peek(uint32_t id) const override {
        if (!validate_id(id)) {
            return nullptr;
        }
        
        static PointData temp_data;
        if (point_locks_[id]->read(temp_data)) {
            return &temp_data;
        }
        
        return nullptr;
    }
    
    bool validate_id(uint32_t id) const override {
        return id < max_points_;
    }
    
    uint64_t get_write_count() const override {
        return 0;
    }
    
    uint64_t get_timeout_count() const override {
        return 0;
    }
    
private:
    std::string instance_id_;
    std::size_t max_points_;
    std::size_t max_subscribers_;
    void* segment_;
    std::unique_ptr<osal::ITime> time_;
    std::vector<std::unique_ptr<ISeqlock>> point_locks_;
};

PointManager* create_point_manager(const std::string& instance_id,
                                  size_t max_points,
                                  size_t max_subscribers) {
    return new (std::nothrow) PointManagerImpl(instance_id, max_points, max_subscribers);
}

void destroy_point_manager(PointManager* manager) {
    delete manager;
}

} // namespace core
} // namespace indurtdb
