#pragma once
#include <atomic>
#include <vector>
#include <list>
#include <functional>
#include <mutex>
#include <thread>

class EbrManager {
private:
    std::atomic<size_t> global_epoch_{0};
    std::list<std::unique_ptr<LocalState>> threads_;
    std::mutex mtx_;

    void try_advance(LocalState* trigger_local) {
        size_t global = global_epoch_.load(std::memory_order_acquire);

        // Check if all active threads caught up to current Epoch
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto& t : threads_) {
            if (t->active.load(std::memory_order_relaxed) &&
                t->epoch.load(std::memory_order_relaxed) != global) {
                return; // Threads lagging; cannot advance
                }
        }

        // Advance (Epoch)
        size_t next = global + 1;
        global_epoch_.store(next, std::memory_order_release);

        // Clean old gens: (Global-2) is safe
        // 3-bucket cycle: (next+1)%3 = oldest bucket
        size_t safe_bin_idx = (next + 1) % 3;

        // Simplified: trigger cleans its own bucket
        // Prod: each thread cleans own bucket or use global cleanup list
        for (auto& t : threads_) {
            auto& bin = t->retire_bins[safe_bin_idx];
            for (auto& node : bin) {
                node.deleter(node.ptr);
            }
            bin.clear();
        }
    }

public:
    struct Node {
        void* ptr;
        std::function<void(void*)> deleter;
    };

    // Thread-local state
    struct LocalState {
        std::atomic<bool> active{false}; // Whether in the critical section
        std::atomic<size_t> epoch{0};    // Globally visible Epoch
        std::vector<Node> retire_bins[3]; // Garbage collection buckets corresponding to 3 generations (epoch-based)
        size_t op_count{0};               // Counter to trigger GC
    };

    static EbrManager& get() {
        static EbrManager instance;
        return instance;
    }

    // Register current thread
    LocalState* register_thread() {
        std::lock_guard<std::mutex> lock(mtx_);
        auto ptr = std::make_unique<LocalState>();
        LocalState* raw = ptr.get();
        threads_.push_back(std::move(ptr));
        return raw;
    }

    // Enter critical section
    void enter(LocalState* local) {
        size_t g = global_epoch_.load(std::memory_order_relaxed);
        local->epoch.store(g, std::memory_order_relaxed);
        local->active.store(true, std::memory_order_seq_cst); // SeqCst barrier
    }

    // Exit critical section
    void exit(LocalState* local) {
        local->active.store(false, std::memory_order_release);
    }

    // Retired ptr (lazy delete)
    template<typename T>
    void retire(LocalState* local, T* ptr) {
        size_t e = global_epoch_.load(std::memory_order_relaxed);
        local->retire_bins[e % 3].push_back({
            ptr, 
            [](void* p) { delete static_cast<T*>(p); }
        });

        local->op_count++;
        if (local->op_count > 64) { // GC every 64 ops
            local->op_count = 0;
            try_advance(local);
        }
    }


};

// RAII guard
struct EbrGuard {
    EbrManager::LocalState* ls;
    EbrGuard(EbrManager::LocalState* s) : ls(s) { EbrManager::get().enter(ls); }
    ~EbrGuard() { EbrManager::get().exit(ls); }
};