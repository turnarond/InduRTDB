/**
 * @file notification_posix.cpp
 * @brief POSIX 通知机制实现（Unix Domain Socket）
 * @version 2.0.0
 */

#include <indurtdb/osal/interface.hpp>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <cstring>
#include <string>
#include <new>

namespace indurtdb {
namespace osal {
namespace posix {

class Notification : public INotification {
public:
    Notification(const std::string& path, bool as_server)
        : socket_path_(path), server_fd_(-1), client_fd_(-1), is_server_(as_server) {}

    ~Notification() override {
        close_fds();
        if (is_server_ && !socket_path_.empty()) {
            unlink(socket_path_.c_str());
        }
    }

    bool initialize() {
        return is_server_ ? setup_server() : setup_client();
    }

    bool send(const void* data, std::size_t size) override {
        int fd = is_server_ ? server_fd_ : client_fd_;
        if (fd < 0) return false;
        ssize_t sent = ::send(fd, data, size, MSG_NOSIGNAL);
        return sent == static_cast<ssize_t>(size);
    }

    bool receive(void* data, std::size_t size, TimestampNs timeout_ns) override {
        int fd = is_server_ ? server_fd_ : client_fd_;
        if (fd < 0) return false;

        if (timeout_ns > 0) {
            struct timeval tv;
            tv.tv_sec  = static_cast<time_t>(timeout_ns / 1000000000ULL);
            tv.tv_usec = static_cast<suseconds_t>((timeout_ns % 1000000000ULL) / 1000ULL);

            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(fd, &readfds);

            int ready = select(fd + 1, &readfds, nullptr, nullptr, &tv);
            if (ready <= 0) return false;
        }

        ssize_t received = ::recv(fd, data, size, 0);
        return received == static_cast<ssize_t>(size);
    }

private:
    std::string socket_path_;
    int server_fd_;
    int client_fd_;
    bool is_server_;

    bool setup_server() {
        server_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (server_fd_ < 0) return false;
        fcntl(server_fd_, F_SETFL, O_NONBLOCK);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);
        unlink(socket_path_.c_str());

        if (bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0
            || listen(server_fd_, 1) < 0) {
            close(server_fd_); server_fd_ = -1;
            return false;
        }
        return true;
    }

    bool setup_client() {
        client_fd_ = socket(AF_UNIX, SOCK_SEQPACKET, 0);
        if (client_fd_ < 0) return false;
        fcntl(client_fd_, F_SETFL, O_NONBLOCK);

        struct sockaddr_un addr;
        memset(&addr, 0, sizeof(addr));
        addr.sun_family = AF_UNIX;
        strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (connect(client_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0
            && errno != EINPROGRESS) {
            close(client_fd_); client_fd_ = -1;
            return false;
        }
        return true;
    }

    void close_fds() {
        if (server_fd_ >= 0) { close(server_fd_); server_fd_ = -1; }
        if (client_fd_ >= 0) { close(client_fd_); client_fd_ = -1; }
    }
};

INotification* create_notification(const std::string& path, bool as_server) {
    auto* n = new (std::nothrow) Notification(path, as_server);
    if (!n) return nullptr;
    if (!n->initialize()) {
        delete n;
        return nullptr;
    }
    return n;
}

void destroy_notification(INotification* notification) {
    delete static_cast<Notification*>(notification);
}

} // namespace posix
} // namespace osal
} // namespace indurtdb
