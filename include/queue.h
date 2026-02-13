// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once
#include "ebr.h"
#include <atomic>
#include <optional>
#include <deque>
#include <mutex>

// 1. Robust GlobalQueue (protected by Mutex)
// Sacrifice minimal theoretical performance for 100% correctness and a green light in TSan
template<typename T>
class GlobalQueue {
private:
    std::deque<T> queue_;
    std::mutex mtx_;
public:
    //Adapt to the raw pointer interface
    bool push_ptr(void* ptr) {
        return push(T::from_address(ptr));
    }

    bool push(T item) {
        std::lock_guard<std::mutex> lock(mtx_);
        queue_.push_back(std::move(item));
        return true;
    }

    std::optional<T> pop() {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.empty()) return std::nullopt;
        T item = std::move(queue_.front());
        queue_.pop_front();
        return item;
    }
};

// Chase-Lev StealQueue (lock-free, high performance preserved)
template<typename T>
class StealQueue {
    struct Array {
        std::atomic<void*>* buffer;
        size_t cap;
        size_t mask;
        Array(size_t c) : cap(c), mask(c - 1) {
            buffer = new std::atomic<void*>[c];
            for(size_t i=0; i<c; ++i) buffer[i].store(nullptr, std::memory_order_relaxed);
        }
        ~Array() { delete[] buffer; }
        void put(size_t i, void* p) { buffer[i & mask].store(p, std::memory_order_relaxed); }
        void* get(size_t i) { return buffer[i & mask].load(std::memory_order_relaxed); }
        Array* resize(long b, long t) {
            Array* new_arr = new Array(cap * 2);
            for (long i = t; i < b; ++i) new_arr->put(i, get(i));
            return new_arr;
        }
    };

    alignas(64) std::atomic<long> top{0};
    alignas(64) std::atomic<long> bottom{0};
    std::atomic<Array*> array;
    EbrManager::LocalState* local_state;

public:
    StealQueue(EbrManager::LocalState* ls) : local_state(ls) {
        array.store(new Array(1024));
    }
    ~StealQueue() { delete array.load(); }

    void push(T item) {
        long b = bottom.load(std::memory_order_relaxed);
        long t = top.load(std::memory_order_acquire);
        Array* a = array.load(std::memory_order_relaxed);

        if (b - t >= (long)a->cap - 1) {
            Array* new_a = a->resize(b, t);
            array.store(new_a, std::memory_order_release);
            EbrManager::get().retire(local_state, a);
            a = new_a;
        }
        //Critical: Task::to_address() is called here, incrementing the reference count
        a->put(b, item.to_address());
        std::atomic_thread_fence(std::memory_order_release);
        bottom.store(b + 1, std::memory_order_relaxed);
    }

    std::optional<T> pop() {
        long b = bottom.load(std::memory_order_relaxed) - 1;
        array.load(std::memory_order_relaxed);
        bottom.store(b, std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        long t = top.load(std::memory_order_seq_cst);

        if (t <= b) {
            Array* a = array.load(std::memory_order_relaxed);
            void* val = a->get(b);
            if (t == b) {
                if (!top.compare_exchange_strong(t, t+1,
                    std::memory_order_seq_cst, std::memory_order_relaxed)) {
                    bottom.store(b + 1, std::memory_order_relaxed);
                    return std::nullopt;
                }
                bottom.store(b + 1, std::memory_order_relaxed);
                // Successfully took the last task
                return T::from_address(val);
            }
            // Successfully took the task
            return T::from_address(val);
        }
        bottom.store(b + 1, std::memory_order_relaxed);
        return std::nullopt;
    }

    std::optional<T> steal() {
        long t = top.load(std::memory_order_acquire);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        long b = bottom.load(std::memory_order_acquire);

        if (t < b) {
            Array* a = array.load(std::memory_order_consume);
            void* val = a->get(t);
            if (!top.compare_exchange_strong(t, t+1,
                std::memory_order_seq_cst, std::memory_order_relaxed)) {
                return std::nullopt;
            }
            return T::from_address(val);
        }
        return std::nullopt;
    }
};