#pragma once

#include "scheduler.h"
#include <sys/socket.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <stdexcept>

// Non-blocking Mode Setting Utility Function
inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// ==========================================
// 1. Awaiters (Wait Body)
// ==========================================

class AsyncReadAwaiter {
    int fd_;
    Reactor* reactor_;
    void* buffer_;
    size_t size_;
    ssize_t result_{0};

public:
    AsyncReadAwaiter(int fd, Reactor* r, void* buf, size_t sz)
        : fd_(fd), reactor_(r), buffer_(buf), size_(sz) {}

    bool await_ready() {
        result_ = ::read(fd_, buffer_, size_);
        if (result_ >= 0) return true;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;
    }

    void await_suspend(std::coroutine_handle<Task::Promise> h) {
        // Ref +1 (for Reactor)
        h.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
        reactor_->register_read(fd_, h.address());
    }

    ssize_t await_resume() {
        if (result_ < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            result_ = ::read(fd_, buffer_, size_);
        }
        return result_;
    }
};

class AsyncWriteAwaiter {
    int fd_;
    Reactor* reactor_;
    const void* buffer_;
    size_t size_;
    ssize_t result_{0};

public:
    AsyncWriteAwaiter(int fd, Reactor* r, const void* buf, size_t sz)
        : fd_(fd), reactor_(r), buffer_(buf), size_(sz) {}

    bool await_ready() {
        result_ = ::write(fd_, buffer_, size_);
        if (result_ >= 0) return true;
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;
    }

    void await_suspend(std::coroutine_handle <Task::Promise> h) {
        // Ref +1 (for Reactor)
        h.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
        reactor_->register_write(fd_, h.address());
    }

    ssize_t await_resume() {
        if (result_ < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            result_ = ::write(fd_, buffer_, size_);
        }
        return result_;
    }
};

// Forward Declaration
class AsyncSocket;

class AsyncAcceptAwaiter {
    int fd_;
    Reactor* reactor_;
    struct sockaddr* addr_;
    socklen_t* len_;
    int client_fd_{-1};

public:
    AsyncAcceptAwaiter(int fd, Reactor* r, struct sockaddr* a, socklen_t* l)
        : fd_(fd), reactor_(r), addr_(a), len_(l) {}

    bool await_ready() {
        client_fd_ = ::accept(fd_, addr_, len_);
        if (client_fd_ >= 0) {
            set_nonblocking(client_fd_);
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
        return true;
    }

    void await_suspend(std::coroutine_handle<Task::Promise> h) {
        // Ref +1 (for Reactor)
        h.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
        reactor_->register_read(fd_, h.address());
    }

    int await_resume() {
        if (client_fd_ < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            client_fd_ = ::accept(fd_, addr_, len_);
            if (client_fd_ >= 0) set_nonblocking(client_fd_);
        }
        return client_fd_;
    }
};

// ==========================================
// 2. AsyncSocket (RAII Wrapper)
// ==========================================

class AsyncSocket {
    int fd_;
    Reactor* reactor_;

public:
    AsyncSocket(int fd, Reactor* r) : fd_(fd), reactor_(r) {
        if (fd_ != -1) set_nonblocking(fd_);
    }

    AsyncSocket(AsyncSocket&& other) noexcept : fd_(other.fd_), reactor_(other.reactor_) {
        other.fd_ = -1;
    }

    AsyncSocket& operator=(AsyncSocket&& other) noexcept {
        if (this != &other) {
            if (fd_ != -1) ::close(fd_); // Use ::close here to prevent recursion
            fd_ = other.fd_;
            reactor_ = other.reactor_;
            other.fd_ = -1;
        }
        return *this;
    }

    AsyncSocket(const AsyncSocket&) = delete;
    AsyncSocket& operator=(const AsyncSocket&) = delete;

    ~AsyncSocket() {
        if (fd_ != -1) ::close(fd_);
    }

    AsyncReadAwaiter read(void* buf, size_t size) {
        return AsyncReadAwaiter(fd_, reactor_, buf, size);
    }

    AsyncWriteAwaiter write(const void* buf, size_t size) {
        return AsyncWriteAwaiter(fd_, reactor_, buf, size);
    }

    AsyncWriteAwaiter write(const std::string& s) {
        return AsyncWriteAwaiter(fd_, reactor_, s.data(), s.size());
    }

    int fd() const { return fd_; }
};

// ==========================================
// 3. TcpListener
// ==========================================

class TcpListener {
    int fd_;
    Reactor* reactor_;

public:
    TcpListener(Reactor* r) : fd_(-1), reactor_(r) {}

    ~TcpListener() {
        if (fd_ != -1) ::close(fd_);
    }

    int bind(const char* ip, int port) {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0) return -1;

        int opt = 1;
        setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        inet_pton(AF_INET, ip, &addr.sin_addr);

        if (::bind(fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0)
            return -1;

        if (::listen(fd_, 4096) < 0)
            return -1;

        set_nonblocking(fd_);
        return 0;
    }

    struct CoAccept {
        AsyncAcceptAwaiter awaiter;
        Reactor* r;

        CoAccept(int fd, Reactor* reactor)
            : awaiter(fd, reactor, nullptr, nullptr), r(reactor) {}

        bool await_ready() { return awaiter.await_ready(); }

        // 🟢 Fix: Forward to Awaiter to increment the reference count
        void await_suspend(std::coroutine_handle<Task::Promise> h) {
            awaiter.await_suspend(h);
        }

        AsyncSocket await_resume() {
            int client_fd = awaiter.await_resume();
            return AsyncSocket(client_fd, r);
        }
    };

    CoAccept accept() {
        return CoAccept(fd_, reactor_);
    }
};