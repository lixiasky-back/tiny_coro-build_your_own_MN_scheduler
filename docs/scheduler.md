# Documentation: include/scheduler.h

## 1. 📄 Overview
**Role**: **The Brain & Heart of the System**.

This file defines the three core classes that constitute the asynchronous runtime:
1.  **`Reactor`**: Responsible for handling I/O events (e.g., incoming network data) and timer events (e.g., `sleep_for`). It is the system's "Heart," continuously generating event-driven pulses.
2.  **`Worker`**: Represents a kernel thread. Its job is to continuously fetch and execute tasks from the queues. It acts as the system's "Muscles."
3.  **`Scheduler`**: Manages all Workers and the Reactor, providing a unified `spawn` interface to the user. It functions as the system's "Brain."

---

## 2. 🏗️ Deep Dive

### 2.1 The `Reactor` Class: I/O and Time Management



```cpp
void loop() {
    auto io_handler = [this](void* udata) {
        if (udata) {
            // Restore void* to Task and throw it into the scheduler
            scheduler_->spawn(Task::from_address(udata));
        }
    };

    while (running_) {
        // 1. Calculate how long until the nearest timer expires
        int timeout = ...; 
        
        // 2. Block and wait for I/O events or timeout
        // This abstracts away the differences between epoll and kqueue
        poller_.wait(timeout, io_handler);

        // 3. Process expired timers
        while (timers_.top().expiry <= now) { ... }
    }
}
```
* **Design Pattern**: A classic implementation of the **Reactor Pattern**.
* **Thread Isolation**: In the current implementation, the Reactor runs on a dedicated thread.
    * **Pros**: Simple logic; I/O processing doesn't block computational tasks (Workers).
    * **Cons**: Distributing I/O events to Workers requires cross-thread communication (involving lock/queue overhead). *(Note: A more aggressive approach is a Per-CPU Reactor where each worker has its own).*

### 2.2 The `Worker` Class: Work-Stealing Loop

```cpp
void run_once() {
    std::optional<Task> task;
    {
        EbrGuard guard(ebr_state_); // Protect lock-free queue operations
        
        // Priority 1: Check own "bowl" (LIFO - hottest data, best cache hit rate)
        if (auto t = local_queue_->pop()) task = std::move(t);
        
        // Priority 2: Check the "community pot" (Global Queue)
        else if (auto t = scheduler_.pop_global()) task = std::move(t);
        
        // Priority 3: If starving, steal from others (Work Stealing)
        else if (auto t = scheduler_.steal()) task = std::move(t);
    }

    if (task) {
        task->resume(); // Execute coroutine until the next suspension point
        return;
    }

    // If no tasks, don't sleep yet—spin for a bit (Busy Loop)
    // Syscall overhead for sleep/wake is high (~μs), tasks might arrive in nanoseconds
    for (int i = 0; i < 50; ++i) { _mm_pause(); ... }

    parker_.park(); // Finally, hibernate to save power
}
```
* **Work Stealing**: The gold standard for modern schedulers.
    * Workers prioritize their **Local Queue** because those tasks were likely just generated, meaning their data is still in the CPU's L1/L2 cache, leading to peak execution speeds.

### 2.3 The `Scheduler` Class: The Public Facade



```cpp
void spawn(Task t) {
    // 1. Task::detach() retrieves the raw pointer, transferring ownership to the queue
    if (void* ptr = t.detach()) {
        global_queue_.push_ptr(ptr);
        
        // 2. Wake a Worker to start processing
        // Uses a simple Round-Robin strategy for selection
        workers_[next++ % workers_.size()]->wake();
    }
}
```
* **`spawn` Ownership Transfer**:
    * When a user calls `spawn(my_coro())`, a temporary `Task` is returned.
    * `t.detach()` strips the internal handle, preventing the coroutine from being destroyed when the `Task` goes out of scope.

---

## 3. 🎓 C++20 Coroutine & Async I/O Spotlight

This section demonstrates how a coroutine suspends itself and registers with the Reactor.

### `AsyncSleep`: The Coroutine Version of Sleep
```cpp
class AsyncSleep {
    bool await_ready() const noexcept { return false; } // Suspend me immediately

    void await_suspend(std::coroutine_handle<> h) {
        auto expiry = now + duration_;
        // Throw the coroutine handle (h) to the Reactor
        sched_.reactor()->add_timer(expiry, h);
    }

    void await_resume() noexcept {} // Nothing to do upon wake
};
```
* **Workflow**:
    1.  User writes `co_await sleep_for(sched, 100);`.
    2.  Coroutine pauses; the Worker thread immediately moves to the next task (**Non-blocking!**).
    3.  Reactor thread adds the handle to a **Min-Heap**.
    4.  100ms later, Reactor detects timeout, retrieves the handle, and calls `scheduler->spawn(handle)`.

---

## 4. 💡 Design Rationale

### 4.1 Why run the Reactor in a separate thread?
* **Model Simplification**: Decouples the I/O event loop from computational tasks.
* **Liveness Guarantee**: If Workers are occupied by CPU-intensive tasks (or legacy blocking code), a dedicated Reactor ensures the system continues to respond to network events.

### 4.2 Why Spin before `park`?
* **Latency Optimization**:
    * `Parker::park` involves a `futex` syscall, forcing a context switch to the kernel which costs roughly **5-10 microseconds**.
    * If a task arrives just **0.1 microseconds** late, sleeping for 10 microseconds is a massive net loss. `_mm_pause()` provides a low-power, cheap waiting mechanism.

### 4.3 The necessity of `Task::detach`
In `spawn`, detaching cuts the link between the stack-allocated object in the main thread and the heap-allocated coroutine frame. This prevents **Reference Count Race Conditions**, where one thread might destroy the coroutine while another is still cleaning up the stack object.