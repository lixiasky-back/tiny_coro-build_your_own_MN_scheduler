// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once
#include <atomic>
#include <thread>

// Lightweight spinlock, to be used with std::lock_guard
struct SpinLock {
    std::atomic<bool> lock_ = {false};

    void lock() {
        while (true) {
            // Attempt to acquire the lock
            if (!lock_.exchange(true, std::memory_order_acquire)) {
                return;
            }
            // Busy waiting (Spin)
            while (lock_.load(std::memory_order_relaxed)) {
#if defined(__x86_64__) || defined(_M_X64)
                _mm_pause();
#elif defined(__aarch64__)
                asm volatile("yield");
#endif
            }
        }
    }

    void unlock() {
        lock_.store(false, std::memory_order_release);
    }
};