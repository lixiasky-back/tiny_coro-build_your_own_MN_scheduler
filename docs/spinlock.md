# Documentation: include/spinlock.h

## 1. 📄 Overview
**Role**: **The High-Speed Gatekeeper**.

In multi-threaded programming, there are generally two types of locks:
1.  **Sleep Locks**: Such as `std::mutex`. If the lock is unavailable, the OS suspends the thread, yielding the CPU to others. This is suitable for long critical sections.
2.  **Spin Locks**: As implemented here. If the lock is unavailable, the thread "spins" in a busy loop, staring at the variable until it turns green.

**Use Case**: **Extremely short** critical sections (nanoseconds to tens of nanoseconds).
In `tiny_coro`, it primarily protects the internal states of the `Parker` or instantaneous queue operations. Since a context switch (sleeping) costs several microseconds, sleeping to wait for a few nanoseconds is a net loss in performance.

---

## 2. 🏗️ Deep Dive

### 2.1 Acquisition: The TTAS Variant (Test-And-Test-And-Set)
The `lock()` function is more than a simple loop; it contains **cache-friendly** optimizations.

```cpp
void lock() {
    while (true) {
        // 1. The actual "grab" (Heavy Operation)
        // RMW (Read-Modify-Write) - can lock the bus or cache line
        if (!lock_.exchange(true, std::memory_order_acquire)) {
            return; // Got it!
        }

        // 2. Only enter this loop if the grab failed
        // This is Read-Only; it doesn't storm the bus
        while (lock_.load(std::memory_order_relaxed)) {
            // ... CPU Pause ...
        }
    }
}
```
* **Why the inner `while`?**
    * `exchange` is an expensive atomic instruction that invalidates the cache lines of other CPU cores. If all waiting threads frantically `exchange`, the bus becomes saturated—a phenomenon known as a **Bus Storm**.
    * The inner `load` is lightweight. If the lock is held, I only read my local cache without disturbing others. Only when I see the lock become `false` do I attempt an `exchange`. This is called **TTAS (Test-Test-And-Set)**.

### 2.2 The Art of Busy-Waiting: CPU Pause
```cpp
#if defined(__x86_64__)
    _mm_pause();
#elif defined(__aarch64__)
    asm volatile("yield");
#endif
```
* **Consequences of omitting `_mm_pause`**:
    * The CPU pipeline fills with speculative instructions because the empty loop executes so quickly.
    * When the lock variable finally changes, the CPU must flush the entire pipeline, incurring a massive performance penalty.
    * Significant heat generation and power waste.
* **Benefits of `_mm_pause`**:
    * Tells the CPU: "I'm waiting for a lock, stop speculating and rest for a few dozen clock cycles."
    * On Hyper-Threading CPUs, it yields resources to the other logical thread on the same physical core.

### 2.3 Releasing (`unlock`)
```cpp
void unlock() {
    // Release semantics:
    // Ensures all modifications made inside the critical section 
    // are visible to others before the lock is released.
    lock_.store(false, std::memory_order_release);
}
```
* Note that we use `store` rather than `exchange`. Since only the lock holder can unlock, there is no contention; a direct write suffices.

---

## 3. 🎓 Technical Spotlight: Cache Line Bouncing



### Plain English: Why is CAS expensive?

Imagine several CPU cores playing ping-pong with a single ball (the cache line).

1.  **Core A** holds the lock (owns the cache line in "Modified" state).
2.  **Core B** attempts an `exchange` (CAS).
    * It must snatch the ball from Core A (Invalidating A's cache).
    * Core A is forced to write the value back or transfer it to B.
    * Core B modifies the value.
3.  **Core C** then attempts an `exchange`.
    * It now has to snatch the ball from Core B.

If everyone is frantically calling `exchange`, the cache line "bounces" between cores constantly. This is **Cache Line Bouncing**, and it can cause performance to drop from nanoseconds to microseconds.
**The inner loop (`load`) stops this tug-of-war; everyone quietly watches the ball. Until the ball hits the ground (the lock is released), no one reaches for it.**

---

## 4. 💡 Design Rationale

### 4.1 Why not just use `std::mutex`?
* `std::mutex` is typically a hybrid lock: it spins a few times and then enters a kernel-level sleep (`futex`).
* While excellent, the overhead of entering the kernel is unacceptable for **micro-granular** operations (like setting a flag in `Parker` or moving a pointer in a queue). We need a solution that stays entirely in User Space.

### 4.2 Why not use `std::atomic_flag`?
* Before C++20, `atomic_flag` was the only type guaranteed to be lock-free.
* However, `std::atomic<bool>` is implemented as lock-free on all modern mainstream CPUs. Its interface (`load`/`store`/`exchange`) is more intuitive and supports finer memory ordering control than `atomic_flag`.

### 4.3 Why use `relaxed` for the `load`?
* In the inner loop, we are merely "observing" if the lock is occupied. We do **not** need to establish a "synchronizes-with" relationship here.
* We only need a memory barrier to ensure the safety of the critical section during the actual "grab" (`exchange` / `acquire`). Using `relaxed` reduces unnecessary memory fence instruction overhead.