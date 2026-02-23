# Documentation: include/task.h

## 1. 📄 Overview
**Role**: **The Atomic Coroutine Container**.

`task.h` is the **foundation** of the entire `tiny_coro` framework. In C++20, a coroutine function must have a return type to interact with the caller. `Task` is this return type, but it is more than just a return value; it is a **smart pointer wrapper for the Coroutine Handle**.

* **Upward (User)**: It is the return value of coroutine functions. Users write `Task my_coro() { ... }`.
* **Downward (Scheduler)**: It is the "cargo" carried by the scheduler queues (Global/Local Queue).
* **Core Functions**:
    1.  **Lifecycle Management**: Uses atomic reference counting to prevent coroutines from being accidentally destroyed during multi-threaded flow.
    2.  **State Control**: Controls the starting (Resume), pausing (Suspend), and termination (Done) of the coroutine.
    3.  **Ownership Transfer**: Allows coroutines to be safely "transferred" between different threads.

---

## 2. 🏗️ Deep Dive

The core difficulty of this file lies in **reference counting** and **atomic operations**, which are designed to adapt to the M:N scheduler.

### 2.1 `struct Promise` (Control Center)
In C++20, the compiler generates a **Coroutine Frame** for each coroutine (usually on the heap). The `Promise` object resides within this frame.

```cpp
struct Promise {
    // Use seq_cst (Sequential Consistency) to ensure absolute safety
    std::atomic<int> ref_count{1};
    std::atomic<bool> is_running{false}; 
    std::coroutine_handle<> continuation = nullptr; // Used to wake up the parent coroutine
    // ...
};
```

* **`ref_count` (Atomic Reference Count)**:
  * **Scenario**: If Thread A puts a task into the queue and its local `Task` object is immediately destructed. Without reference counting, the coroutine frame might be released instantly. When Thread B retrieves the pointer from the queue, it would access a wild pointer.
  * **`seq_cst`**: Guarantees that all threads see the same order of increment/decrement. While there is a minor overhead, it provides **absolute memory safety**.

* **`is_running` (Reentrancy Lock)**:
  * **Purpose**: Prevents two threads from calling `resume` on the same coroutine simultaneously. Although the scheduler logic should avoid this, this layer of defense prevents segmentation faults caused by illegal operations.

### 2.2 `Task` Class (Smart Pointer)
`Task` is essentially a manual implementation of `std::shared_ptr`, specifically tailored for coroutine handles.

* **Key Method: `detach()`**
    ```cpp
    void* detach() {
        void* ptr = handle ? handle.address() : nullptr;
        handle = nullptr; // Critical! Relinquish ownership; destructor will no longer decrement count
        return ptr;
    }
    ```
  * **Principle**: When a task is pushed into a **lock-free queue**, the queue only stores a `void*`. By calling `detach`, we transfer the coroutine's lifecycle management from the stack-based `Task` object to the queue (maintained by the counter).

---

## 3. 🎓 Coroutine Hooks

### 3.1 `initial_suspend`: The Frozen Game Character
```cpp
std::suspend_always initial_suspend() noexcept { return {}; }
```
* **Analysis**: The coroutine is **suspended immediately** after creation.
* **Design Rationale**: **Work-Stealing friendly**. The main thread is only responsible for "producing" tasks and placing them in the queue, not for immediate execution. Suspending it allows worker threads to "steal" the task via load-balancing algorithms.

### 3.2 `FinalAwaiter`: Relay and Symmetric Transfer
This is the logic executed after the coroutine finishes its last line of code.

```cpp
struct FinalAwaiter {
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        h.promise().is_running.store(false, ...);
        // If a parent coroutine is waiting for me, transfer control directly to it
        if (h.promise().continuation) return h.promise().continuation;
        return std::noop_coroutine();
    }
};
```
* **Advanced Feature: Symmetric Transfer**:
  * **Principle**: Note that this returns a `coroutine_handle` rather than `void`. This tells the compiler: "I am finished, please **jump** directly to the next coroutine."
  * **Advantage**: This is a standard feature for high-performance coroutine libraries. It implements **Tail Call Optimization**. Without it, nested coroutine calls (A waits for B, B waits for C) would cause the call stack to deepen continuously, eventually leading to a stack overflow. With it, the call stack remains only one level deep regardless of nesting.

---

## 4. 💡 Design Rationale

### 4.1 Why support `co_await Task`?
* **Implementation**: `operator co_await`.
* **Scenario**: Allows coroutines to be composed like regular functions, e.g., `co_await socket.read()`.
* **Logic**: When a `parent` waits for a `child`, the `parent` suspends itself and stores its handle in the `child`'s `continuation`. When the `child` finishes, the relay mechanism mentioned in Section 3.2 seamlessly wakes up the `parent` to resume execution.

### 4.2 Why only use `std::suspend_always`?
* **Philosophy**: **Non-blocking design**. A coroutine should never preempt the execution right of the current thread upon creation. By forcing an initial suspension, we ensure all execution rights are uniformly allocated by the `Scheduler`.

### 4.3 Memory Management Safety
* **Reference Count Reaching Zero**: `handle.destroy()` is executed to release coroutine frame memory only when the last object holding the `Task` is destructed and no queues hold the handle address. This completely resolves memory barrier and visibility issues in lock-free scheduling.

