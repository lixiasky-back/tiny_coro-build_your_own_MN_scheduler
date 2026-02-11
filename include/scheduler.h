#pragma once

#include <vector>
#include <thread>
#include <atomic>
#include <memory>
#include <random>
#include <optional>
#include <mutex>
#include <condition_variable>
#include <queue>

// ✅ Introduce cross-platform Poller (encapsulates epoll/kqueue)
#include "poller.h"

// Introduce basic components
#include "task.h"
#include "ebr.h"
#include "queue.h"
#include "parker.h"

// ==========================================
// 1. Basic Components
// ==========================================

using TimePoint = std::chrono::steady_clock::time_point;

struct Timer {
    TimePoint expiry;
    std::coroutine_handle<> handle;
    bool operator>(const Timer& other) const {
        return expiry > other.expiry;
    }
};

// Forward declaration
class Scheduler;
class Worker;

// ==========================================
// 2. Reactor Definition
// ==========================================
class Reactor {
private:
    Scheduler* scheduler_;
    Poller poller_; // Use the cross-platform Poller class from poller.h
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::mutex mtx_;
    std::priority_queue<Timer, std::vector<Timer>, std::greater<Timer>> timers_;

    void loop();
public:
    explicit Reactor(Scheduler* sched);
    ~Reactor();

    void start();
    void stop();
    void add_timer(TimePoint expiry, std::coroutine_handle<> handle);

    // ✅ Proxy Poller's registration interface
    // Poller internally handles epoll/kqueue differences automatically
    void register_read(int fd, void* handle) { poller_.add_read(fd, handle); }
    void register_write(int fd, void* handle) { poller_.add_write(fd, handle); }


};

// ==========================================
// 3. Worker Definition
// ==========================================
class Worker {
private:
    size_t id_;
    Scheduler& scheduler_;
    EbrManager::LocalState* ebr_state_;
    std::unique_ptr<StealQueue<Task>> local_queue_;
    Parker parker_;
    std::mt19937 rng_;
    void run_once();

public:
    Worker(size_t id, Scheduler& s);
    Worker(const Worker&) = delete;

    void run();
    void wake() { parker_.unpark(); }
    void schedule(Task t);
    std::optional<Task> steal();
    size_t id() const { return id_; }


};

// ==========================================
// 4. Scheduler Definition
// ==========================================
class Scheduler {
private:
    friend class Worker;
    GlobalQueue<Task> global_queue_;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::vector<std::thread> threads_;
    std::atomic<bool> stop_{false};
    std::unique_ptr<Reactor> reactor_;

public:
    Scheduler(size_t n = std::thread::hardware_concurrency());
    ~Scheduler();

    // 🟢 Spawn: Use detach() to avoid reference count race conditions
    void spawn(Task t) {
        if (void* ptr = t.detach()) {
            global_queue_.push_ptr(ptr);
            if (!workers_.empty()) {
                static std::atomic<size_t> next{0};
                workers_[next.fetch_add(1, std::memory_order_relaxed) % workers_.size()]->wake();
            }
        }
    }

    std::optional<Task> pop_global() { return global_queue_.pop(); }

    std::optional<Task> steal() {
        size_t n = workers_.size();
        if (n <= 1) return std::nullopt;
        static thread_local std::mt19937 rng(std::random_device{}());
        size_t start = std::uniform_int_distribution<size_t>(0, n-1)(rng);
        for (size_t i = 0; i < n; ++i) {
            size_t idx = (start + i) % n;
            if (auto t = workers_[idx]->steal()) return t;
        }
        return std::nullopt;
    }

    size_t worker_count() const { return workers_.size(); }
    Worker& get_worker(size_t i) { return *workers_[i]; }
    bool is_running() const { return !stop_.load(std::memory_order_acquire); }
    Reactor* reactor() { return reactor_.get(); }
};

// ==========================================
// 5. Implementation Details
// ==========================================

// --- Reactor Implementation ---

inline Reactor::Reactor(Scheduler* sched) : scheduler_(sched) {}
inline Reactor::~Reactor() { stop(); }

inline void Reactor::start() {
    if (!running_) {
        running_ = true;
        thread_ = std::thread([this] { loop(); });
    }
}

inline void Reactor::stop() {
    if (running_.exchange(false)) {
        poller_.wake(); // Call Poller's wake-up mechanism (eventfd or kevent)
        if (thread_.joinable()) thread_.join();
    }
}

inline void Reactor::add_timer(TimePoint expiry, std::coroutine_handle<> handle) {
    bool need_wake = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        timers_.push({expiry, handle});
        if (timers_.top().expiry == expiry) need_wake = true;
    }
    if (need_wake) poller_.wake();
}

inline void Reactor::loop() {
    // ✅ Define IO callback function
    // Whether using Linux epoll or macOS kqueue, the underlying layer will call this lambda
    auto io_handler = [this](void* udata) {
        if (udata) {
            // Recover Task from void* and schedule it
            // Note: Task::from_address takes over the reference count
            scheduler_->spawn(Task::from_address(udata));
        }
    };

    while (running_) {
        int timeout_ms = -1;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (!timers_.empty()) {
                auto now = std::chrono::steady_clock::now();
                auto next = timers_.top().expiry;
                if (next <= now) timeout_ms = 0;
                else timeout_ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(next - now).count());
            }
        }

        // ✅ Core change: unified wait interface
        // Pass timeout and callback to mask differences in underlying event arrays
        poller_.wait(timeout_ms, io_handler);

        // Handle timers
        {
            std::lock_guard<std::mutex> lock(mtx_);
            auto now = std::chrono::steady_clock::now();
            while (!timers_.empty() && timers_.top().expiry <= now) {
                Timer t = timers_.top();
                timers_.pop();
                scheduler_->spawn(Task::from_address(t.handle.address()));
            }
        }
    }
}

// --- Scheduler & Worker Implementation (logic remains unchanged) ---

inline Scheduler::Scheduler(size_t n) {
    reactor_ = std::make_unique<Reactor>(this);
    workers_.reserve(n);
    for (size_t i = 0; i < n; ++i) workers_.push_back(std::make_unique<Worker>(i, *this));
    reactor_->start();
    for (size_t i = 0; i < n; ++i) threads_.emplace_back([this, i] { workers_[i]->run(); });
}

inline Scheduler::~Scheduler() {
    if (reactor_) reactor_->stop();
    stop_.store(true, std::memory_order_release);
    for(auto& w : workers_) w->wake();
    for(auto& t : threads_) if(t.joinable()) t.join();
}

inline Worker::Worker(size_t id, Scheduler& s)
    : id_(id), scheduler_(s), rng_(std::random_device{}()) {
    ebr_state_ = EbrManager::get().register_thread();
    local_queue_ = std::make_unique<StealQueue<Task>>(ebr_state_);
}

inline void Worker::schedule(Task t) {
    local_queue_->push(std::move(t));
}

inline std::optional<Task> Worker::steal() {
    return local_queue_->steal();
}

inline void Worker::run() {
    while (scheduler_.is_running()) {
        run_once();
    }
}

inline void Worker::run_once() {
    std::optional<Task> task;
    {
        EbrGuard guard(ebr_state_);
        if (auto t = local_queue_->pop()) task = std::move(t);
        else if (auto t = scheduler_.pop_global()) task = std::move(t);
        else if (auto t = scheduler_.steal()) task = std::move(t);
    }

    if (task) {
        task->resume();
        return;
    }

    for (int i = 0; i < 50; ++i) {
        {
            EbrGuard guard(ebr_state_);
            if (auto t = scheduler_.pop_global()) {
                 t->resume();
                 return;
            }
        }
        #if defined(__x86_64__) || defined(_M_X64)
            _mm_pause();
        #elif defined(__aarch64__)
            asm volatile("yield");
        #endif
    }
    parker_.park();
}

// Helper Classes
class AsyncSleep {
private:
    Scheduler& sched_;
    std::chrono::milliseconds duration_;
public:
    AsyncSleep(Scheduler& s, std::chrono::milliseconds d) : sched_(s), duration_(d) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) {
        auto expiry = std::chrono::steady_clock::now() + duration_;
        sched_.reactor()->add_timer(expiry, h);
    }
    void await_resume() noexcept {}
};

inline AsyncSleep sleep_for(Scheduler& s, int ms) {
    return AsyncSleep(s, std::chrono::milliseconds(ms));
}