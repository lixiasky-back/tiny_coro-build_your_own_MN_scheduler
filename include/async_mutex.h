// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once

#include "scheduler.h"
#include <queue>
#include <mutex>
#include <atomic>

class AsyncMutex {
public:
    // Constructor: requires a Scheduler to wake up the waiter
    explicit AsyncMutex(Scheduler& sched) : sched_(sched), locked_(false) {}

    // Disable copy and assignment
    AsyncMutex(const AsyncMutex&) = delete;
    AsyncMutex& operator=(const AsyncMutex&) = delete;

    // --- RAII Guard definition ---
    class ScopedLock {
        AsyncMutex& mutex_;
        bool owns_lock_;

    public:
        ScopedLock(AsyncMutex& m) : mutex_(m), owns_lock_(true) {}

        // Support move construction, disable copy
        ScopedLock(ScopedLock&& other) noexcept : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
            other.owns_lock_ = false;
        }

        ScopedLock(const ScopedLock&) = delete;
        ScopedLock& operator=(const ScopedLock&) = delete;

        ~ScopedLock() {
            if (owns_lock_) {
                mutex_.unlock();
            }
        }
    };

    // --- Lock operation Awaiter ---
    struct LockAwaiter {
        AsyncMutex& mutex;

        // 1. Fast Path (Quick Path)
        // Try to acquire the lock directly without suspension
        bool await_ready() {
            std::lock_guard<std::mutex> lock(mutex.wait_mtx_);
            if (!mutex.locked_) {
                mutex.locked_ = true;
                return true; // Lock acquired successfully, no suspension
            }
            return false; // Lock is occupied, enter await_suspend
        }

        // 2. Slow Path (Slow Path)
        // Return bool instead of void to handle race conditions
        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(mutex.wait_mtx_);

            // ✅ Double-Check
            // At the very moment before we prepare to suspend, the coroutine holding the lock may have called unlock().
            // If the lock becomes free at this moment, we must seize it immediately and must not suspend!
            if (!mutex.locked_) {
                mutex.locked_ = true;
                return false; // Return false to cancel suspension and resume execution immediately
            }

            // The lock is indeed still occupied, queue up and suspend
            mutex.waiters_.push(h);
            return true; // Return true to confirm suspension
        }

        // 3. Resume
        // When we wake up, or when await_ready returns true, it means we already hold the lock
        ScopedLock await_resume() {
            return ScopedLock{mutex};
        }
    };

    // Acquire lock (coroutine version)
    LockAwaiter lock() {
        return LockAwaiter{*this};
    }

    // Release lock
    void unlock() {
        std::lock_guard<std::mutex> lock(wait_mtx_);

        if (waiters_.empty()) {
            // No one is waiting, release the state directly
            locked_ = false;
        } else {
            // ⚠️ Baton Passing (Relay Baton Mechanism)
            // Someone is waiting, we **do not** set locked_ to false.
            // Instead, keep locked_ = true and wake up the next waiter directly.
            // When the next waiter wakes up (in await_resume), it naturally owns the lock.
            auto h = waiters_.front();
            waiters_.pop();

            // Throw the awakened task back to the scheduler
            sched_.spawn(Task::from_address(h.address()));
        }
    }

private:
    Scheduler& sched_;
    bool locked_;                 // Logical lock state
    std::mutex wait_mtx_;         // Mutex for protecting internal state (spinlock granularity)
    std::queue<std::coroutine_handle<>> waiters_; // Wait queue
};