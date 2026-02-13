// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once

#include <vector>
#include <stdexcept>
#include <unistd.h>

// ==========================================
// Linux Implementation (epoll)
// ==========================================
#ifdef __linux__
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <cstring> // for memset

class Poller {
    int epoll_fd_;
    int wake_fd_;
    struct epoll_event events_[128];

public:
    Poller() {
        epoll_fd_ = epoll_create1(EPOLL_CLOEXEC);
        if (epoll_fd_ == -1) throw std::runtime_error("epoll_create1 failed");

        // Create an eventfd for wake-up
        wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (wake_fd_ == -1) throw std::runtime_error("eventfd failed");

        // Add wake_fd to epoll
        struct epoll_event ev = {};
        ev.events = EPOLLIN | EPOLLET;
        ev.data.fd = wake_fd_;
        epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, wake_fd_, &ev);
    }

    ~Poller() {
        close(wake_fd_);
        close(epoll_fd_);
    }

    // Wake-up: write data to eventfd
    void wake() {
        uint64_t val = 1;
        ::write(wake_fd_, &val, sizeof(val));
    }

    // Register read: use EPOLLONESHOT to simulate OneShot behavior
    void add_read(int fd, void* udata) {
        struct epoll_event ev = {};
        ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
        ev.data.ptr = udata;
        // MOD if exists, otherwise ADD (handle Keep-Alive)
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            if (errno == EEXIST) {
                epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
            }
        }
    }

    void add_write(int fd, void* udata) {
        struct epoll_event ev = {};
        ev.events = EPOLLOUT | EPOLLONESHOT | EPOLLRDHUP;
        ev.data.ptr = udata;
        if (epoll_ctl(epoll_fd_, EPOLL_CTL_ADD, fd, &ev) < 0) {
            if (errno == EEXIST) {
                epoll_ctl(epoll_fd_, EPOLL_CTL_MOD, fd, &ev);
            }
        }
    }

    // Unified Wait interface: pass a callback to handle events
    // Callback: void(void* udata)
    template<typename F>
    int wait(int timeout_ms, F&& callback) {
        int n = epoll_wait(epoll_fd_, events_, 128, timeout_ms);
        for (int i = 0; i < n; ++i) {
            
            if (events_[i].data.ptr == nullptr) {
                // This is wake_fd: read the data to empty the buffer
                uint64_t val;
                ::read(wake_fd_, &val, sizeof(val));
                continue; // No callback, continue directly
            }

            // Regular IO events
            callback(events_[i].data.ptr);
        }
        return n;
    }
};

// ==========================================
// macOS Implementation (kqueue)
// ==========================================
#elif defined(__APPLE__) || defined(__FreeBSD__)
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>

class Poller {
    int kq_;
    int wakeup_id_ = 42;
    struct kevent events_[128];

public:
    Poller() {
        kq_ = kqueue();
        if (kq_ == -1) throw std::runtime_error("kqueue create failed");
        struct kevent ev;
        EV_SET(&ev, wakeup_id_, EVFILT_USER, EV_ADD | EV_CLEAR, 0, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

    ~Poller() {
        if (kq_ != -1) close(kq_);
    }

    void wake() {
        struct kevent ev;
        EV_SET(&ev, wakeup_id_, EVFILT_USER, 0, NOTE_TRIGGER, 0, nullptr);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

    void add_read(int fd, void* udata) {
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_READ, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, udata);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

    void add_write(int fd, void* udata) {
        struct kevent ev;
        EV_SET(&ev, fd, EVFILT_WRITE, EV_ADD | EV_ENABLE | EV_ONESHOT, 0, 0, udata);
        kevent(kq_, &ev, 1, nullptr, 0, nullptr);
    }

    template<typename F>
    int wait(int timeout_ms, F&& callback) {
        struct timespec ts;
        struct timespec* tsp = nullptr;
        if (timeout_ms >= 0) {
            ts.tv_sec = timeout_ms / 1000;
            ts.tv_nsec = (timeout_ms % 1000) * 1000000;
            tsp = &ts;
        }

        int n = kevent(kq_, nullptr, 0, events_, 128, tsp);
        for (int i = 0; i < n; ++i) {
            if (events_[i].filter == EVFILT_USER) continue;
            
            if (events_[i].udata) {
                callback(events_[i].udata);
            }
        }
        return n;
    }
};
#endif