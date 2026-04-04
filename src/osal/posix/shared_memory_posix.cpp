/**
 * @file shared_memory_posix.cpp
 * @brief POSIX共享内存实现
 * @version 1.0.0
 * @date 2026-03-27
 * @copyright MIT License
 */

#include <indurtdb/osal/interface.hpp>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <string>
#include <stdexcept>

namespace indurtdb {
namespace osal {
namespace posix {

class SharedMemory : public ISharedMemory {
public:
    SharedMemory(const std::string& name) 
        : name_(name), fd_(-1), mapped_(nullptr), size_(0), owner_(false) {}
    
    ~SharedMemory() override {
        unmap();
    }
    
    void* map(std::size_t size) override {
        if (mapped_) {
            return mapped_;
        }
        
        size_ = size;
        
        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            return nullptr;
        }
        
        if (ftruncate(fd_, size_) < 0) {
            close(fd_);
            fd_ = -1;
            return nullptr;
        }
        
        mapped_ = mmap(nullptr, size_, PROT_READ | PROT_WRITE, 
                      MAP_SHARED, fd_, 0);
        if (mapped_ == MAP_FAILED) {
            close(fd_);
            fd_ = -1;
            mapped_ = nullptr;
            return nullptr;
        }
        
        struct stat st;
        if (fstat(fd_, &st) == 0 && st.st_size == 0) {
            owner_ = true;
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
    
    bool is_owner() const override {
        return owner_;
    }
    
    std::size_t size() const override {
        return size_;
    }
    
private:
    std::string name_;
    int fd_;
    void* mapped_;
    std::size_t size_;
    bool owner_;
};

ISharedMemory* create_shared_memory(const std::string& name) {
    // 不使用异常处理，直接返回new的结果
    return new (std::nothrow) SharedMemory(name);
}

void destroy_shared_memory(ISharedMemory* mem) {
    delete static_cast<SharedMemory*>(mem);
}

} // namespace posix
} // namespace osal
} // namespace indurtdb

// extern "C" 工厂函数
extern "C" {
    indurtdb::osal::ISharedMemory* create_shared_memory(const char* name) {
        return indurtdb::osal::posix::create_shared_memory(name);
    }
    
    void destroy_shared_memory(indurtdb::osal::ISharedMemory* mem) {
        indurtdb::osal::posix::destroy_shared_memory(mem);
    }
}