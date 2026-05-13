/**
 * @file indurtdb_impl.cpp
 * @brief InduRTDB C++ API 实现 —— 集成 SharedMemorySegment + PointManager + SubscriptionManager
 * @version 2.0.0
 * @date 2026-05-11
 * @copyright MIT License
 */

#include <indurtdb/api/indurtdb.hpp>
#include <indurtdb/core/point_manager_interface.hpp>
#include <indurtdb/core/subscription_manager_interface.hpp>
#include <indurtdb/core/shared_memory_segment.hpp>
#include <indurtdb/core/config_loader.hpp>
#include <indurtdb/osal/factory.hpp>
#include <cstdio>
#include <cstring>
#include <unistd.h>

namespace indurtdb {

// ============================================================
// InduRTDB::Impl —— 内部实现
// ============================================================

class InduRTDB::Impl {
public:
    Impl() : initialized_(false), seg_(nullptr), pm_(nullptr), sm_(nullptr),
             time_(nullptr) {}

    ~Impl() { shutdown(); }

    bool initialize(const char* instance_id,
                    uint32_t max_points,
                    uint32_t max_subscribers) {
        if (initialized_) return false;
        if (!instance_id || instance_id[0] == '\0') return false;

        // 1. 获取 OSAL 时间
        time_ = osal::OSALFactory::create_time();
        if (!time_) return false;

        // 2. 创建共享内存段
        seg_ = new (std::nothrow) core::SharedMemorySegment(
            instance_id, max_points, max_subscribers);
        if (!seg_) return false;

        if (!seg_->initialize()) {
            delete seg_; seg_ = nullptr;
            time_.reset();
            return false;
        }

        // 3. 创建 PointManager（直接操作共享内存）
        pm_ = new (std::nothrow) core::PointManager(
            seg_->base(), max_points, time_.get());
        if (!pm_) {
            seg_->shutdown(); delete seg_; seg_ = nullptr;
            time_.reset();
            return false;
        }

        // 4. 创建 SubscriptionManager
        sm_ = new (std::nothrow) core::SubscriptionManager(
            time_.get(), seg_->subscribers(), max_subscribers);
        if (!sm_) {
            delete pm_; pm_ = nullptr;
            seg_->shutdown(); delete seg_; seg_ = nullptr;
            time_.reset();
            return false;
        }

        initialized_ = true;
        return true;
    }

    // ---- 写入 ----

    bool write(PointId id, bool value) {
        if (!initialized_) return false;
        bool ok = pm_->write(id, value);
        if (ok) sm_->notify(id, *pm_->peek(id));
        return ok;
    }

    bool write(PointId id, int32_t value) {
        if (!initialized_) return false;
        bool ok = pm_->write(id, value);
        if (ok) sm_->notify(id, *pm_->peek(id));
        return ok;
    }

    bool write(PointId id, double value) {
        if (!initialized_) return false;
        bool ok = pm_->write(id, value);
        if (ok) sm_->notify(id, *pm_->peek(id));
        return ok;
    }

    bool write(PointId id, const char* value) {
        if (!initialized_) return false;
        bool ok = pm_->write(id, value);
        if (ok) sm_->notify(id, *pm_->peek(id));
        return ok;
    }

    // ---- 读取 ----

    bool read(PointId id, PointData& out) const {
        if (!initialized_) return false;
        return pm_->read(id, out);
    }

    const PointData* peek(PointId id) const {
        if (!initialized_) return nullptr;
        return pm_->peek(id);
    }

    // ---- 订阅 ----

    bool subscribe(PointId id, SubscriptionCallback cb, void* user_data) {
        if (!initialized_) return false;
        return sm_->subscribe(id, cb, user_data);
    }

    bool unsubscribe(PointId id) {
        if (!initialized_) return false;
        return sm_->unsubscribe(id);
    }

    // ---- 配置 ----

    bool load_config(const char* config_path) {
        if (!initialized_) return false;
        if (!config_path) return false;

        core::ConfigResult result{};
        if (!core::parse_point_config(config_path, result)) {
            std::fprintf(stderr, "[InduRTDB] Failed to parse config: %s\n",
                         config_path);
            return false;
        }

        // 将解析出的点位写入共享内存（初始化元数据）
        for (size_t i = 0; i < result.count; ++i) {
            const auto& pc = result.points[i];
            if (!pm_->validate_id(pc.id)) continue;

            PointData* p = pm_->points() + pc.id;
            p->type   = static_cast<PointType>(pc.type);
            p->unit   = static_cast<Unit>(pc.unit);
            p->access = static_cast<Access>(pc.access);
            std::strncpy(p->name, pc.name, sizeof(p->name) - 1);
            p->name[sizeof(p->name) - 1] = '\0';
        }

        core::free_config_result(result);
        return true;
    }

    // ---- 心跳 ----

    void update_heartbeat() {
        if (!initialized_) return;
        pid_t pid = getpid();
        sm_->update_heartbeat(static_cast<Pid>(pid));
    }

    // ---- 生命周期 ----

    bool is_initialized() const { return initialized_; }

    void shutdown() {
        if (!initialized_) return;

        delete sm_; sm_ = nullptr;
        delete pm_; pm_ = nullptr;

        if (seg_) {
            seg_->shutdown();
            delete seg_;
            seg_ = nullptr;
        }

        time_.reset();
        initialized_ = false;
    }

private:
    bool initialized_;

    core::SharedMemorySegment* seg_;
    core::PointManager*        pm_;
    core::SubscriptionManager* sm_;

    std::unique_ptr<osal::ITime> time_;
};

// ============================================================
// InduRTDB 外层代理
// ============================================================

InduRTDB::InduRTDB() : impl_(new Impl()) {}
InduRTDB::~InduRTDB() { delete impl_; }

InduRTDB& InduRTDB::instance() {
    static InduRTDB inst;
    return inst;
}

bool InduRTDB::initialize(const char* instance_id,
                          uint32_t max_points,
                          uint32_t max_subscribers) {
    return impl_->initialize(instance_id, max_points, max_subscribers);
}

// 显式模板特化
template<> bool InduRTDB::write<bool>(PointId id, const bool& v)
    { return impl_->write(id, v); }
template<> bool InduRTDB::write<int32_t>(PointId id, const int32_t& v)
    { return impl_->write(id, v); }
template<> bool InduRTDB::write<double>(PointId id, const double& v)
    { return impl_->write(id, v); }

// 字符串 —— 非模板重载，避免字面量类型推导问题
bool InduRTDB::write(PointId id, const char* value)
    { return impl_->write(id, value); }

bool InduRTDB::read(PointId id, PointData& out) const
    { return impl_->read(id, out); }

const PointData* InduRTDB::peek(PointId id) const
    { return impl_->peek(id); }

bool InduRTDB::subscribe(PointId id, SubscriptionCallback cb, void* user_data)
    { return impl_->subscribe(id, cb, user_data); }

bool InduRTDB::unsubscribe(PointId id)
    { return impl_->unsubscribe(id); }

bool InduRTDB::load_config(const char* config_path)
    { return impl_->load_config(config_path); }

void InduRTDB::update_heartbeat()
    { impl_->update_heartbeat(); }

bool InduRTDB::is_initialized() const
    { return impl_->is_initialized(); }

void InduRTDB::shutdown()
    { impl_->shutdown(); }

} // namespace indurtdb
