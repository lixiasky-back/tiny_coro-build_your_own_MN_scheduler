# Documentation: include/ebr.h

## 1. 📄 Overview
**Role**: **The Memory "Night's Watch" for Lock-Free Algorithms (Garbage Collector)**.

In lock-free programming, we cannot simply use `delete` to release node memory.
* **Core Conflict**: Thread A is reading Node X while Thread B removes Node X from the linked list. If Thread B executes `delete X` immediately, Thread A will access a wild pointer (Use-After-Free), leading to a crash.
* **The Solution (EBR)**:
    * When Thread B removes Node X, it doesn't delete it directly; instead, it posts it to a **"Retired"** queue.
    * EBR acts as an observer, monitoring the progress of all worker threads.
    * Only after **all** threads that could possibly hold a reference to Node X have left the current "Epoch" will EBR truly deallocate the physical memory.



---

## 2. 🏗️ Deep Dive

EBR's core logic is based on a global counter known as **"Three-Epoch Cycling."**

### 2.1 Data Structure: `LocalState`
Each thread has its own Thread-Local state block:
```cpp
struct LocalState {
    std::atomic<bool> active{false};  // Flag: Am I currently operating on shared data?
    std::atomic<size_t> epoch{0};     // Perspective: Which global epoch am I observing?
    std::vector<Node> retire_bins[3]; // Garbage bins: Corresponding to three epochs (mod 3)
};
```
* **Role of `active`**: Key to performance optimization. When a thread is processing non-shared data (e.g., pure computation), it is marked `false`. This prevents EBR from waiting for that thread to advance the global epoch, avoiding the "long-tail effect."

### 2.2 Entering/Exiting Critical Sections (`enter` / `exit`)
```cpp
void enter(LocalState* local) {
    size_t g = global_epoch_.load(std::memory_order_relaxed);
    local->epoch.store(g, std::memory_order_relaxed);
    
    // Critical Barrier: SeqCst (Sequential Consistency)
    // This is a "global announcement" ensuring the fact "I am active" is visible to all, 
    // and preventing instruction reordering.
    local->active.store(true, std::memory_order_seq_cst); 
}

void exit(LocalState* local) {
    // Release: Ensures all read/write operations finish before I leave the critical section
    local->active.store(false, std::memory_order_release);
}
```

### 2.3 Attempting to Advance the Epoch (`try_advance`)
This is the trigger for garbage collection, using an "aggressive cleanup" strategy.
```cpp
void try_advance(LocalState* trigger_local) {
    size_t global = global_epoch_.load(std::memory_order_acquire);

    // 1. Global Check: Are there any "active" threads lagging behind?
    for (auto& t : threads_) {
        if (t->active.load(std::memory_order_relaxed) && 
            t->epoch.load(std::memory_order_relaxed) != global) {
            return; // Someone is stuck in an old epoch; abort GC for safety
        }
    }

    // 2. Advance Epoch: All active threads have caught up; enter the new era
    size_t next = global + 1;
    global_epoch_.store(next, std::memory_order_release);

    // 3. Clean "Grandparent" garbage (Current -> Prev -> Safe)
    // (next + 1) % 3 points to the bin from (Global - 2), which is absolutely safe.
    size_t safe_bin_idx = (next + 1) % 3;
    
    // Note: This logic holds a global lock to help all threads clean this specific bin
    for (auto& t : threads_) {
        // ... Perform delete operations ...
    }
}
```

---

## 3. 🎓 C++20 Memory Model Spotlight

This file demonstrates a textbook application of C++ memory orders, specifically the Store-Load barrier.

### 3.1 Why must `active.store` be `seq_cst`?
```cpp
local->active.store(true, std::memory_order_seq_cst);
```
* **Plain English**: This is an **Iron Gate**.
* **Danger Scenario (Store-Load Reordering)**:
    * Without `seq_cst`, the CPU might optimize performance by executing "Read shared data pointer" *before* "Mark `active=true`".
    * **Consequence**: The thread grabs the data pointer before officially announcing it is active. The GC thread sees "Oh, he's inactive" and deletes the data. The thread then accesses the pointer -> **Crash**.
* **Role of SeqCst**: Forces the CPU to follow a strict order—**you must raise your hand (Active=true) before you can touch the data.**

### 3.2 Why three garbage bins?
We need three bins to accommodate different security levels of garbage:
* **$G$ (Current Epoch)**: New garbage being produced. Threads may be referencing it.
* **$G-1$ (Previous Epoch)**: The epoch just switched. Lagging threads may still be here referencing these objects. **Dangerous, do not delete.**
* **$G-2$ (Safe Epoch)**: Since the global epoch has moved to $G$, it means all active threads have at least seen $G-1$. Therefore, no thread can possibly see objects in $G-2$. **Absolutely safe to delete.**



---

## 4. 💡 Rationale & Limitations

### 4.1 Why not use `std::shared_ptr`?
* **Performance Bottleneck**: `shared_ptr` relies on atomic reference counting. Under high multi-core concurrency, every pointer copy triggers bus locking or cache line bouncing, causing performance to tank.
* **EBR Advantage**: Readers have **zero atomic contention** (only reading/writing local variables). Only writers bear the GC overhead. It is ideal for **Read-Heavy** scenarios.

### 4.2 RAII Guard (`EbrGuard`)
* **Design Rationale**: Prevents exception safety issues. If a thread calls `enter()` but an exception skips `exit()`, that thread remains "Active" forever. This stalls global epoch advancement, preventing all memory reclamation and leading to Out-of-Memory (OOM). RAII ensures the active status is cleared on any exit path.

### 4.3 ⚠️ Known Limitation: Thread Lifecycle Management
* **Zombie Thread Risk**: In the current implementation, if a thread is destroyed after `register_thread`, its `LocalState` remains in the list.
* **Consequence**: While `active` defaulting to `false` won't block GC, the garbage left in that thread's `retire_bins` will never be cleaned, causing a slow memory leak.
* **Suggestion**: Production environments should implement an `unregister_thread` mechanism to transfer residual garbage to a global pool upon thread exit.