// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once

#include "scheduler.h"
#include <queue>
#include <mutex>
#include <optional>

template <typename T>
class Channel {
public:
    // Constructor: Need to pass in Scheduler to reschedule when waking up coroutines
    Channel(Scheduler& sched, size_t capacity = 0) 
        : sched_(sched), capacity_(capacity), closed_(false) {}

    // Disable copy and move (for simplicity, Channel is usually shared via pointers or references)
    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    ~Channel() { close(); }

    void close() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (closed_) return;
        closed_ = true;
        
        // Wake up all waiting senders and receivers
        while (!send_waiters_.empty()) {
            auto& w = send_waiters_.front();
            sched_.spawn(Task::from_address(w.handle.address()));
            send_waiters_.pop();
        }
        while (!recv_waiters_.empty()) {
            auto& w = recv_waiters_.front();
            // Set a null value or error flag to let receivers know it's closed
            if (w.result_ptr) *w.result_ptr = std::nullopt;
            sched_.spawn(Task::from_address(w.handle.address()));
            recv_waiters_.pop();
        }
    }

    bool is_closed() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return closed_;
    }

    // --- Send Awaitable ---
    struct SendAwaiter {
        Channel& chan;
        T value;
        bool result = true; // Whether the send operation succeeded

        // Always let the control flow enter await_suspend and make safe judgments within the lock
        bool await_ready() { return false; }

        // Return value (bool):
        // false -> Do not suspend, resume the current coroutine immediately (high-performance path)
        // true  -> Suspend, transfer control of the current coroutine to the scheduler (blocking path)
        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(chan.mtx_);

            // 0. Channel is closed
            if (chan.closed_) {
                result = false;
                return false; // Do not suspend, return failure directly
            }

            // 1. Direct Handoff (Directly to waiting receivers)
            // Whether the buffer is full or empty, if there are receivers waiting, send directly to them and skip the buffer
            if (!chan.recv_waiters_.empty()) {
                auto& waiter = chan.recv_waiters_.front();
                // Write data directly across stacks
                *waiter.result_ptr = std::move(value);

                // Wake up the receiver (via the scheduler)
                chan.sched_.spawn(Task::from_address(waiter.handle.address()));
                chan.recv_waiters_.pop();

                // ✅ The current sender does not suspend and continues execution directly
                return false;
            }

            // 2. The buffer is not full
            if (chan.buffer_.size() < chan.capacity_) {
                chan.buffer_.push(std::move(value));
                // ✅ Write succeeded, no suspension, continue execution directly
                return false;
            }

            // 3. Blocking suspension
            // The buffer is full and there are no receivers, so suspension is mandatory
            chan.send_waiters_.push({h, &value});
            return true;
        }

        bool await_resume() { return result; }
    };

    // --- Recv Awaitable ---
    struct RecvAwaiter {
        Channel& chan;
        std::optional<T> result;

        bool await_ready() { return false; }

        // Return bool: Optimization logic is the same as above
        bool await_suspend(std::coroutine_handle<> h) {
            std::lock_guard<std::mutex> lock(chan.mtx_);

            // 1. Prioritize reading from the buffer
            if (!chan.buffer_.empty()) {
                result = std::move(chan.buffer_.front());
                chan.buffer_.pop();

                // The buffer has free space; check if there are any senders waiting
                if (!chan.send_waiters_.empty()) {
                    auto& sender = chan.send_waiters_.front();
                    // Chain operation: Move the sender's data into the newly freed slot
                    chan.buffer_.push(std::move(*sender.value_ptr));
                    // Wake up the sender
                    chan.sched_.spawn(Task::from_address(sender.handle.address()));
                    chan.send_waiters_.pop();
                }

                // ✅ Data received, no suspension, return directly
                return false;
            }

            // 2. The buffer is empty but there are senders waiting (for capacity=0 cases)
            // Direct Handoff: Grab data directly from the sender
            if (!chan.send_waiters_.empty()) {
                auto& sender = chan.send_waiters_.front();
                result = std::move(*sender.value_ptr);

                // Wake up the sender
                chan.sched_.spawn(Task::from_address(sender.handle.address()));
                chan.send_waiters_.pop();

                // ✅ Data received, no suspension
                return false;
            }

            // 3. If closed and no data available
            if (chan.closed_) {
                result = std::nullopt;
                return false; // No suspension, return a null value
            }

            // 4. No data available, suspend
            chan.recv_waiters_.push({h, &result});
            return true;
        }

        std::optional<T> await_resume() { return std::move(result); }
    };

    // User interface
    SendAwaiter send(T val) { return SendAwaiter{*this, std::move(val)}; }
    RecvAwaiter recv() { return RecvAwaiter{*this}; }

private:
    struct SenderWaiter {
        std::coroutine_handle<> handle;
        T* value_ptr; // Points to the value in SendAwaiter
    };

    struct RecvWaiter {
        std::coroutine_handle<> handle;
        std::optional<T>* result_ptr; // Points to the result in RecvAwaiter
    };

    Scheduler& sched_;
    size_t capacity_;
    bool closed_;
    
    mutable std::mutex mtx_;
    std::queue<T> buffer_;
    std::queue<SenderWaiter> send_waiters_;
    std::queue<RecvWaiter> recv_waiters_;
};