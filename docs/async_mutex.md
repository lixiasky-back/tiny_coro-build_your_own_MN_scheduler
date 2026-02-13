# Documentation: include/async_mutex.h

## 1. 📄 Overview
**Role**: **The Traffic Light for Coroutines**.

In an M:N scheduling model (like `tiny_coro`), you must **never** use `std::mutex` to protect critical sections.
* **Why?** `std::mutex` blocks the current **Physical Thread (Worker Thread)**. If a Worker is blocked, the entire scheduler's throughput plummets, potentially leading to a system deadlock.
* **The AsyncMutex Solution**: It is **cooperative**. When a lock is occupied, it doesn't block the thread; instead, it **suspends the current coroutine**, freeing the physical thread to execute other tasks. Once the lock is released, the coroutine is pushed back into the scheduling queue.

---

## 2. 🏗️ Deep Dive

### 2.1 Internal Protection: `wait_mtx_`
You might ask: *"Didn't you say we can't use `std::mutex`? Why is there one in the code?"*
* **Distinguishing Two Levels**:
    1.  **Logic Layer (User Land)**: `AsyncMutex` protects user business logic (e.g., modifying a Map), which may be held for "long" periods (milliseconds).
    2.  **Implementation Layer (Kernel Land)**: `wait_mtx_` protects the internal `waiters_` queue of the `AsyncMutex`, held for extremely short durations (nanoseconds).
* **Conclusion**: Using `std::mutex` at the implementation layer to protect the queue is perfectly fine. It only prevents race conditions during push/pop operations and won't cause long-term blocking.

### 2.2 Locking Logic: The Critical Double-Check
`LockAwaiter` implements the standard Awaitable pattern and fixes the most common **Lost Wakeup** problem in coroutine locks.

#### Fast Path (`await_ready`)
```cpp
bool await_ready() {
    std::lock_guard<std::mutex> lock(mutex.wait_mtx_);
    if (!mutex.locked_) {
        mutex.locked_ = true;
        return true; // ⚡️ Success! Unoccupied, enter directly without suspending.
    }
    return false; // Lock occupied, prepare to suspend.
}
```

#### Slow Path (`await_suspend`)

```cpp
bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(mutex.wait_mtx_);

    // ✅ Double-Check
    // Scenario: In the tiny gap between returning false from await_ready and entering await_suspend,
    // the previous holder might have just called unlock().
    // Without this check, we would sleep on an "idle lock," causing a deadlock.
    if (!mutex.locked_) {
        mutex.locked_ = true; // Grab the lock
        return false;         // ❌ Cancel suspension, resume execution immediately!
    }

    // Lock is indeed occupied, join the queue
    mutex.waiters_.push(h);
    return true; // ✅ Confirm suspension
}
```

### 2.3 Unlocking Logic: Baton Passing
This is the **soul** of this file and the key to high performance.

```cpp
void unlock() {
    std::lock_guard<std::mutex> lock(wait_mtx_);

    if (waiters_.empty()) {
        locked_ = false; // No one waiting, release state
    } else {
        // ⚠️ Key Point: Baton Passing
        // We DO NOT set locked_ to false.
        // Instead, we pop a waiter and wake it up directly.
        auto h = waiters_.front();
        waiters_.pop();
        
        // The awakened coroutine wakes up assuming it already holds the lock
        // (because locked_ is still true).
        sched_.spawn(Task::from_address(h.address()));
    }
}
```

---

## 3. 🎓 Core Concept: Baton Passing

### Plain English: What is Baton Passing?

Imagine a public restroom (critical section) and a line of people waiting.

* **Non-Baton Passing (Preemptive)**:
    * The person inside comes out and shouts: "It's empty!" (`locked = false`).
    * Before the person at the front of the line can react, a bystander (new coroutine) rushes in and takes it.
    * **Consequence**: Unfair; people in line might stay there forever (Starvation), and high competition leads to a "Thundering Herd" effect.

* **Baton Passing (Relay)**:
    * The person inside comes out and looks at the line.
    * They **do not** unlock the door (`locked` remains `true`).
    * Instead, they hand the key directly to the first person in line: "Buddy, it's yours."
    * Any new bystander sees the door is "locked" and joins the back of the line.
    * **Advantages**:
        1.  **Absolute Fairness**: Strict FIFO (First-In, First-Out).
        2.  **High Performance**: The awakened coroutine doesn't need to perform another CAS (Compare-And-Swap) to grab the lock; it wakes up already owning it.

---

## 4. 💡 Design Rationale

### 4.1 Why depend on the `Scheduler`?
```cpp
explicit AsyncMutex(Scheduler& sched) : sched_(sched) ...
```
* When `unlock` wakes a waiter, we call `sched_.spawn(h)` instead of `h.resume()`.
* **Prevent Stack Overflow**: If A unlocks and wakes B, and B executes and unlocks to wake C... direct resumption would cause the call stack to deepen indefinitely.
* **Load Balancing**: The awakened coroutine should be thrown back into the global queue to be shared by all Workers, rather than being pinned to the current unlocking thread.

### 4.2 Why use `ScopedLock`?
```cpp
class ScopedLock { ~ScopedLock() { mutex_.unlock(); } };
```
* **RAII (Resource Acquisition Is Initialization)**: Coroutine code can have branches and exceptions.
* If a user manually calls `lock()` and `unlock()`, the lock might never be released if an exception is thrown or an early `co_return` occurs.
* `ScopedLock` guarantees the lock is released regardless of how the coroutine ends, effectively eliminating **deadlocks**.