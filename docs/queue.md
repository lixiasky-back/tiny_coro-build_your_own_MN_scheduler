# Documentation: include/queue.h

## 1. 📄 Overview
**Role**: **The "Logistics and Sorting Center" for Tasks**.

In the M:N scheduling model, to balance **correctness under high concurrency** with **low overhead for single-core execution**, `tiny_coro` adopts a dual-layer queue architecture:

1.  **`GlobalQueue` (Global Drop-off Station)**:
    * **Nature**: A robust queue protected by `std::mutex`.
    * **Purpose**: Receives new tasks generated externally (e.g., from the `main` function) or acts as a buffer during task overflows.
    * **Design Philosophy**: **Safety First**. Since the frequency of tasks entering the global queue is much lower than inter-thread scheduling, using a mutex ensures 100% memory visibility guarantees.

2.  **`StealQueue` (Local Private/Stealing Queue)**:
    * **Nature**: A lock-free, Single-Producer Multi-Consumer (SPMC) queue based on the **Chase-Lev Algorithm**.
    * **Purpose**: Private to each worker thread. Threads prioritize their own tasks and only "steal" from other threads when they are "starving."
    * **Design Philosophy**: **Extreme Performance**. This is the lifeline of system throughput; lock contention must be eliminated through lock-free mechanisms.

---

## 2. 🏗️ Deep Dive

### 2.1 `StealQueue`: The Core of Lock-Free Concurrency



#### 2.1.1 False Sharing Prevention
```cpp
alignas(64) std::atomic<long> top{0};    // Head: Thieves read from here
alignas(64) std::atomic<long> bottom{0}; // Tail: Owner accesses from here
```
* **Analysis**: Modern CPU cache lines are typically 64 bytes. If `top` and `bottom` reside on the same cache line, the Owner updating `bottom` would invalidate the Thief's cache line, triggering expensive bus traffic.
* **Solution**: Use `alignas(64)` to force them onto different cache lines, enabling true parallel read/write operations.

#### 2.1.2 Why is EBR required during Resize?
```cpp
if (b - t >= (long)a->cap - 1) {
    Array* new_a = a->resize(b, t);
    array.store(new_a, std::memory_order_release);
    EbrManager::get().retire(local_state, a); // Critical!
}
```
* **Challenge**: When the Owner switches to a new array, a Thief might have just read the `void*` pointer to the old array and is preparing to parse the task address.
* **Solution**: `retire(a)` does not immediately destroy the memory. EBR ensures that `delete a` is only executed after all threads currently performing a "steal" action have left their critical sections. This solves the classic **"Premature Memory Reclamation"** problem in lock-free programming.

#### 2.1.3 The Decisive Moment in `pop()`
The most subtle part of the `pop` logic is handling the competition for the "last remaining element":
```cpp
if (t == b) {
    // Both Owner (Pop) and Thief (Steal) reach for the same task
    if (!top.compare_exchange_strong(t, t+1, ...)) {
        // CAS Failed: The Thief won; the task was stolen.
        bottom.store(b + 1, ...);
        return std::nullopt;
    }
    // CAS Success: The Owner won; successfully reclaimed the last task.
    bottom.store(b + 1, ...);
    return T::from_address(val);
}
```
* **Memory Barriers**: The heavy use of `std::atomic_thread_fence(std::memory_order_seq_cst)` is to prevent **Store-Load reordering**. It must be guaranteed that the sequence "increment/decrement index, then read content" is visible across all platforms.

---

## 3. 🎓 Core Principle: Why is Chase-Lev Correct?

### 3.1 Fatal Store-Load Reordering
In `pop()`, there is a critical segment:
```cpp
bottom.store(b, std::memory_order_relaxed); // 1. Write Bottom
std::atomic_thread_fence(std::memory_order_seq_cst); // 2. Strong Fence
long t = top.load(std::memory_order_seq_cst); // 3. Read Top
```
* **Without this Fence**:
    * For pipeline efficiency, modern CPUs might reorder instructions, executing step 3 (Read Top) before step 1 (Write Bottom).
    * **Consequence**: The Owner reads an old `top` (thinking the queue is not empty), while a Thief has just stolen the last element and updated `top`. The Owner, unaware, proceeds to fetch data, leading to **Double Consumption**.
* **Dekker's Algorithm Principle**: This logic is equivalent to the classic Dekker’s mutual exclusion algorithm. The `seq_cst` fence forces the Store Buffer to flush, ensuring "I modified bottom" is visible to the entire system before "I read top."

### 3.2 The Sole Contention Point: Empty-1 State
The StealQueue is an SPMC model; 99% of the time, the Owner and Thieves operate on different pointers without interference.
* **Conflict Condition**: Only when the **remaining element count is 1** (`t == b`).
* **Race Scenario**:
    1. Owner tries to `pop`, decrementing `bottom`.
    2. Thief tries to `steal`, incrementing `top`.
    3. Without atomic protection, both could succeed, leading to `top > bottom` (index corruption).
* **Defensive Line**: In `pop`, when `t == b` is detected, we **do not take the data directly**. Instead, we switch to using `compare_exchange_strong` on `top`. In this final moment, the Owner effectively becomes a "thief" as well, competing fairly with actual Thieves.



---

## 4. 💡 Design Rationale

### 4.1 Why not use `std::shared_ptr` to manage the Array?
* **Scenario**: Managing the lifecycle of old arrays during Resize.
* **Flaw of `shared_ptr`**: `shared_ptr` reference counting is atomic. Using it would require an atomic increment/decrement every time `push/pop/steal` reads the `array` pointer.
* **Performance Killer**: This leads to **Cache Line Ping-Pong**. Multiple cores fight for ownership of the same counter's cache line, resulting in performance worse than a locked queue.
* **EBR Advantage**: EBR allows for **Zero-Overhead Reads**. Reading is just reading a pointer—no write operations—making it extremely cache-friendly.

### 4.2 Task Ownership Conversion (`to_address` / `from_address`)
* **Storing**: `item.to_address()` internally increments the `Task` reference count. The coroutine frame is now marked as "in transit" within the logistics system.
* **Retrieving**: `T::from_address(val)` wraps the raw pointer back into a `Task` smart object.
* **Conclusion**: This mechanism ensures that **even if a task is stolen by another thread, its memory lifecycle remains safe** until the actual executor destroys it.

### 4.3 Why not use lock-free queues for everything?
While lock-free queues are incredibly fast for internal scheduling, they suffer from severe performance degradation during **external task submission** (Multi-Producer writes) due to high contention.
* **Strategy**: **Divide and Conquer**. External tasks go to the `GlobalQueue` (robust, low-frequency), while internal movement uses `StealQueue` (extreme speed, high-frequency).