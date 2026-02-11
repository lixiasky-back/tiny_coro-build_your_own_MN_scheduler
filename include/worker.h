#pragma once
#include "ebr.h"
#include "queue.h"
#include "task.h"
#include "parker.h"
#include <random>
#include <thread>
#include <memory>

class Scheduler;

class Worker {
    size_t id_;
    Scheduler& scheduler_;
    EbrManager::LocalState* ebr_state_;
    std::unique_ptr<StealQueue<Task>> local_queue_;
    Parker parker_;
    std::mt19937 rng_;

public:
    Worker(size_t id, Scheduler& s);

    // Disable copy
    Worker(const Worker&) = delete;

    void run();
    void wake() { parker_.unpark(); }
    void schedule(Task t) { local_queue_->push(std::move(t)); }

    // For stealing by other threads
    std::optional<Task> steal() { return local_queue_->steal(); }
    size_t id() const { return id_; }

private:
    void run_once();
    std::optional<Task> find_task();
};