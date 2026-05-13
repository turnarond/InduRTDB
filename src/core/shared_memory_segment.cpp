/**
 * @file shared_memory_segment.cpp
 * @brief SharedMemorySegment 实现
 * @version 2.1.0
 */

#include <indurtdb/core/shared_memory_segment.hpp>
#include <indurtdb/osal/factory.hpp>
#include <cstdio>
#include <cstring>

namespace indurtdb {
namespace core {

static constexpr uint32_t MAGIC   = 0x1DBA1DBA;
static constexpr uint32_t VERSION = 1;

SharedMemorySegment::SharedMemorySegment(
    const char* instance_id, uint32_t max_points, uint32_t max_subscribers)
    : base_(nullptr), total_size_(0), is_owner_(false),
      header_(nullptr), points_(nullptr), subscribers_(nullptr),
      max_points_(max_points), max_subscribers_(max_subscribers)
{
    int n = snprintf(name_, sizeof(name_), "/indurtdb_%s", instance_id);
    if (n < 0 || static_cast<size_t>(n) >= sizeof(name_)) {
        name_[0] = '\0';
    }

    total_size_ = sizeof(InduRTDBHeader)
                + max_points_ * sizeof(PointData)
                + max_subscribers_ * sizeof(SubscriberEntry);
}

SharedMemorySegment::~SharedMemorySegment() {
    shutdown();
}

bool SharedMemorySegment::initialize() {
    if (name_[0] == '\0') return false;

    shm_ = osal::OSALFactory::create_shared_memory(name_);
    if (!shm_) return false;

    void* addr = shm_->map(total_size_);
    if (!addr) {
        shm_.reset();
        return false;
    }

    is_owner_ = shm_->is_owner();
    base_ = addr;

    header_      = static_cast<InduRTDBHeader*>(base_);
    points_      = reinterpret_cast<PointData*>(
                     static_cast<char*>(base_) + sizeof(InduRTDBHeader));
    subscribers_ = reinterpret_cast<SubscriberEntry*>(
                     static_cast<char*>(base_)
                     + sizeof(InduRTDBHeader)
                     + max_points_ * sizeof(PointData));

    if (is_owner_) {
        init_header();
    } else if (!validate_header()) {
        base_ = nullptr; header_ = nullptr;
        points_ = nullptr; subscribers_ = nullptr;
        shm_.reset();
        return false;
    }
    return true;
}

void SharedMemorySegment::shutdown() {
    base_ = nullptr;
    header_ = nullptr;
    points_ = nullptr;
    subscribers_ = nullptr;
    shm_.reset();  // 触发 munmap + close + (is_owner_ ? shm_unlink : 0)
}

void* SharedMemorySegment::base() const { return base_; }
InduRTDBHeader*  SharedMemorySegment::header() const { return header_; }
PointData*       SharedMemorySegment::points() const { return points_; }
SubscriberEntry* SharedMemorySegment::subscribers() const { return subscribers_; }
uint32_t SharedMemorySegment::max_points() const { return max_points_; }
uint32_t SharedMemorySegment::max_subscribers() const { return max_subscribers_; }
size_t   SharedMemorySegment::total_size() const { return total_size_; }
bool     SharedMemorySegment::is_owner() const { return is_owner_; }

void SharedMemorySegment::init_header() {
    std::memset(base_, 0, total_size_);
    header_->magic           = MAGIC;
    header_->version         = VERSION;
    header_->max_points      = max_points_;
    header_->max_subscribers = max_subscribers_;
    header_->write_seq       = 0;
    header_->stats.writes    = 0;
    header_->stats.timeouts  = 0;
}

bool SharedMemorySegment::validate_header() const {
    if (header_->magic != MAGIC) {
        std::fprintf(stderr, "[InduRTDB] magic mismatch: expected 0x%08X got 0x%08X\n",
                     MAGIC, header_->magic);
        return false;
    }
    if (header_->version != VERSION) {
        std::fprintf(stderr, "[InduRTDB] version mismatch: expected %u got %u\n",
                     VERSION, header_->version);
        return false;
    }
    return true;
}

} // namespace core
} // namespace indurtdb
