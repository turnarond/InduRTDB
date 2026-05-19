/**
 * @file shared_memory_posix.cpp
 * @brief POSIX 共享内存实现
 * @version 2.0.0
 */

#include <indurtdb/osal/interface.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <new>

namespace indurtdb {
namespace osal {
namespace posix {

class SharedMemory : public ISharedMemory {
public:
    explicit SharedMemory(const std::string& name)
        : name_(name), fd_(-1), mapped_(nullptr), size_(0), owner_(false) {}

    ~SharedMemory() override { unmap(); }

    void* map(std::size_t size) override {
        if (mapped_) return mapped_;
        size_ = size;

        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) return nullptr;

        // Detect ownership: if the segment was just created (size was 0)
        struct stat st;
        if (fstat(fd_, &st) == 0 && st.st_size == 0) {
            owner_ = true;
        }

        if (owner_ && ftruncate(fd_, static_cast<off_t>(size_)) < 0) {
            close(fd_); fd_ = -1;
            return nullptr;
        }

        mapped_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE,
                       MAP_SHARED, fd_, 0);
        if (mapped_ == MAP_FAILED) {
            close(fd_); fd_ = -1;
            mapped_ = nullptr;
            return nullptr;
        }
        return mapped_;
    }

    void unmap() override {
        if (mapped_ && size_ > 0) {
            munmap(mapped_, size_);
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        if (owner_) {
            shm_unlink(name_.c_str());
        }
        mapped_ = nullptr;
        size_ = 0;
        owner_ = false;
    }

    bool is_owner() const override { return owner_; }
    std::size_t size() const override { return size_; }

private:
    std::string name_;
    int fd_;
    void* mapped_;
    std::size_t size_;
    bool owner_;
};

ISharedMemory* create_shared_memory(const std::string& name) {
    return new (std::nothrow) SharedMemory(name);
}

void destroy_shared_memory(ISharedMemory* mem) {
    delete static_cast<SharedMemory*>(mem);
}

} // namespace posix
} // namespace osal
} // namespace indurtdb
