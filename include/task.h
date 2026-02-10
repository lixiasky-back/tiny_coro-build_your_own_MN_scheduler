#pragma once
#include <coroutine>
#include <atomic>
#include <exception>

struct Task {
    struct Promise {
        // Use seq_cst for absolute safety
        std::atomic<int> ref_count{1};
        std::atomic<bool> is_running{false};
        std::coroutine_handle<> continuation = nullptr;

        Task get_return_object();
        std::suspend_always initial_suspend() noexcept { return {}; }

        struct FinalAwaiter {
            bool await_ready() noexcept { return false; }
            std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
                h.promise().is_running.store(false, std::memory_order_seq_cst);
                if (h.promise().continuation) return h.promise().continuation;
                return std::noop_coroutine();
            }
            void await_resume() noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::terminate(); }
    };

    using promise_type = Promise;
    std::coroutine_handle<Promise> handle;

    struct AdoptTag {}; // Expose Tag

    Task() : handle(nullptr) {}

    explicit Task(std::coroutine_handle<Promise> h) : handle(h) {
        if (handle) handle.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
    }

    // Take over construction (unchanged)
    Task(std::coroutine_handle<Promise> h, AdoptTag) : handle(h) {}

    // Copy (+1)
    Task(const Task& o) : handle(o.handle) {
        if (handle) handle.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
    }

    // Move (unchanged)
    Task(Task&& o) noexcept : handle(o.handle) { o.handle = nullptr; }

    Task& operator=(Task&& o) noexcept {
        if (this != &o) {
            if (handle) dec_ref();
            handle = o.handle;
            o.handle = nullptr;
        }
        return *this;
    }

    ~Task() { if (handle) dec_ref(); }

    void dec_ref() {
        if (handle.promise().ref_count.fetch_sub(1, std::memory_order_seq_cst) == 1) {
            handle.destroy();
        }
    }

    // 🟢 Core fix: Separate the handle and transfer ownership
    // Return a raw pointer and set the internal handle to null at the same time.
    // The caller must take responsibility for managing the lifecycle of this returned pointer (e.g., putting it into a queue).
    // This operation does NOT involve any reading/writing of ref_count!
    void* detach() {
        void* ptr = handle ? handle.address() : nullptr;
        handle = nullptr; // Relinquish ownership: the destructor will no longer call dec_ref
        return ptr;
    }

    // Compatible interface: only for obtaining the address, no ownership transfer
    void* to_address() {
        if (handle) {
            handle.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
            return handle.address();
        }
        return nullptr;
    }

    // Restore
    static Task from_address(void* ptr) {
        if (!ptr) return Task();
        return Task(std::coroutine_handle<Promise>::from_address(ptr), AdoptTag{});
    }

    void resume() {
        if (!handle || handle.done()) return;
        bool expected = false;
        if (handle.promise().is_running.compare_exchange_strong(expected, true, std::memory_order_seq_cst)) {
            handle.resume();
            if (handle && !handle.done()) {
                handle.promise().is_running.store(false, std::memory_order_seq_cst);
            }
        }
    }
    bool done() const { return !handle || handle.done(); }
};

inline Task Task::Promise::get_return_object() {
    return Task{std::coroutine_handle<Promise>::from_promise(*this), Task::AdoptTag{}};
}