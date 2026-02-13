# Documentation: include/parker.h

## 1. 📄 Overview
**Role**: **The "Hibernation Chamber" for Threads**.

`Parker` is a primitive used for thread synchronization. Its role is simple yet vital:
* **park()**: When a worker thread finds the queue empty and has no tasks to steal, it calls this method to put itself to **"sleep"**, ceasing CPU consumption.
* **unpark()**: When a new task is spawned, the scheduler calls this method to **"wake up"** the sleeping thread.

It is significantly **lighter** than the traditional `std::mutex` + `std::condition_variable` combination, as it is based directly on the C++20 atomic wait mechanism (typically mapped to the OS Futex).

---

## 2. 🏗️ Deep Dive

This class maintains a simple **three-state state machine** to solve the classic "Lost Wakeup" problem in concurrent programming.



### 2.1 State Definitions
```cpp
enum State { 
    EMPTY = 0,    // Initial state: Nothing happening
    PARKED = 1,   // Parked: Thread is sleeping (or preparing to)
    NOTIFIED = 2  // Notified: Someone woke it up (even if it wasn't asleep yet)
};
```

### 2.2 `park()`: Attempting to Sleep
```cpp
void park() {
    int expected = EMPTY;
    // 1. CAS Attempt: Only mark as PARKED if current state is EMPTY
    // memory_order_acquire: Ensures I see the task data written by others upon waking
    if (state.compare_exchange_strong(expected, PARKED, std::memory_order_acquire)) {
        
        // 2. Wait Loop: Prevents "Spurious Wakeup"
        while (state.load(std::memory_order_acquire) == PARKED) {
            // 3. Core: C++20 Atomic Wait
            // "If state is still PARKED, suspend current thread until notified"
            // This yields the CPU and enters a kernel wait state
            state.wait(PARKED);
        }
    }
    // 4. Reset to EMPTY after waking, ready for the next use
    state.store(EMPTY, std::memory_order_release);
}
```
* **Logic Details**:
    * If the state is already `NOTIFIED` (e.g., someone called `unpark` just now), the `compare_exchange` fails. The thread **skips sleeping and goes straight to work**. This handles "premature notifications."
    * `state.wait(PARKED)`: This places the thread in the OS wait queue, dropping CPU usage to 0%.

### 2.3 `unpark()`: The Wake-up Call
```cpp
void unpark() {
    // 1. Atomic Exchange: Force state to NOTIFIED regardless of current value
    // memory_order_release: Guarantees task data I wrote is visible before waking others
    int old = state.exchange(NOTIFIED, std::memory_order_release);
    
    // 2. Only invoke the OS-level wakeup if the previous state was PARKED
    if (old == PARKED) {
        state.notify_one(); // Knock on the door and wake it up
    }
}
```
* **Logic Details**:
    * If `old == EMPTY`: The state is changed to `NOTIFIED`. If the thread tries to `park()` later, it will see the `NOTIFIED` state and stay awake.
    * If `old == PARKED`: The thread is already asleep (or about to be); `notify_one()` is called to trigger the wake-up.

---

## 3. 🎓 C++20 Core Concept: Atomic Wait

This is one of the most significant improvements to concurrent programming in C++20.



### Plain English: What is `std::atomic::wait`?

* **The Old Way (Condition Variable)**:
  Imagine you are waiting for a package.
    * **Old Method**: You sit by the door and check the mailbox every few minutes (Polling), or you buy a heavy lock (Mutex), lock the door, and sleep. The mailman must have a key, unlock the door, and then knock. This is "heavy" with high overhead.

* **The New Way (Atomic Wait / Futex)**:
    * **New Method**: You stick a post-it note on the door (Atomic Variable).
    * `wait(VAL)` means: "If the note says VAL, I sleep; otherwise, I don't."
    * The mailman changes the number on the note and shouts (`notify`).
    * **Advantage**: **No Mutex required**. Most checks happen in User Space; you only enter Kernel Space when you truly need to sleep. It's incredibly fast.

---

## 4. 💡 Design Rationale

### 4.1 Why include a `NOTIFIED` state?
This prevents the **Lost Wakeup** issue.

* **Scenario**:
    1. Thread A decides to sleep after checking an empty queue.
    2. **At that exact microsecond** (before A calls `wait`), Thread B pushes a task and calls `unpark()`.
    3. If not handled, B's signal is sent while A isn't yet "listening." When A finally calls `wait`, it misses the signal and sleeps forever despite tasks being available.
* **The Solution**:
  Thread B's `unpark` sets the state to `NOTIFIED` first. When A tries to sleep, the CAS check sees `NOTIFIED` instead of `EMPTY`, so A **aborts the sleep** and processes the task.

### 4.2 Memory Ordering (Acquire/Release)
* **`store(..., release)`**: Used in `unpark`. Ensures that the task data I added to the queue is fully written to memory before the other thread is woken up.
* **`load(..., acquire)`**: Used when waking in `park`. Ensures I see the most recent task data written by others.
* This creates a **Synchronizes-with** relationship, preventing data tearing or reordering.

### 4.3 Why not use `std::binary_semaphore`?
While C++20 provides semaphores, `Parker` logic is custom-tailored: we need an automatic reset to `EMPTY` upon finishing `park`, and precise control over how `NOTIFIED` overrides the sleep attempt. Hand-rolling atomic wait offers the highest performance ceiling with zero abstraction overhead.