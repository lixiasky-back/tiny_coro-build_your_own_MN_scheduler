# 🔍 Deep Dive: The Definitive Guide to Epoch-Based Reclamation (EBR)

> **Warning**: This chapter ventures into the deep waters of multi-threaded memory models.
> If you find standard `mutex` locks difficult, think of EBR as directing traffic at a busy intersection with no traffic lights.
> Master this, and you master the essence of lock-free programming.

---

## 1. Why Do We Need EBR?

### 💀 The Fundamental Conflict: Lock-Free Reads vs. Dynamic Deletion
In lock-free data structures (like our `StealQueue`), **Readers** do not use locks to ensure maximum speed. This means **nothing stops a reader from accessing any node at any time.**

**Scenario: The ABA Nightmare**
1.  **Thread A (Reader)**: Obtains a pointer to Node X and prepares to read its data.
    * *Context switch occurs; Thread A is paused.*
2.  **Thread B (Writer)**: Decides to delete Node X. It removes X from the list and immediately calls `delete X`.
3.  **Thread B (Writer)**: Allocates a new Node Y. The OS happens to assign Y the exact same memory address X just occupied (since the memory is "hot").
4.  **Thread A (Reader)**: Wakes up. It still holds the address of X. It thinks it is reading X, but it is actually accessing Y's data.
    * **Outcome**: Logical errors (dirty data) or crashes (segmentation faults if Y's structure differs).

**Conclusion**: In lock-free programming, **as long as any thread might hold a pointer to an old node, you absolutely cannot release that memory.**

---

## 2. Core Logic: The Three Eras

EBR (Epoch-Based Reclamation) uses the concept of **"Deferred Reclamation."** It doesn't delete garbage immediately; it waits for the "Era" to change.



### 🌍 Global Epoch ($G$)
The current "world time," e.g., $G=100$.

### 🎫 Local Epoch ($L$)
The "entry ticket" held by each thread.
* **Active**: The thread is inside a critical section. Its $L$ must equal $G$.
* **Inactive**: The thread is doing something else (sleeping, calculating hashes). It holds no dangerous pointers.

### 🗑️ The Three Limbo Lists
We maintain three "trash cans" corresponding to different eras:
* **Current ($G$)**: Garbage generated in the current era.
* **Previous ($G-1$)**: Garbage from the last era. **(The Danger Zone)**
* **Safe ($G-2$)**: Garbage from two eras ago. **(The Safe Zone)**

---

## 3. Algorithm Deduction: Why is G-2 Safe?

Let’s derive the safety logic. Suppose the current Global Epoch is $G=100$.

1.  **Current State**:
    * Because $G=100$, any **new** thread entering the critical section gets a ticket for $100$.
    * However, some **lagging** threads might have entered at $G=99$ and haven't left yet. They might still see garbage from the $99$ era.

2.  **Advancement Condition**:
    * The collector checks all threads.
    * It finds: **All active threads now have tickets for 100!** (Meaning no one is stuck in the 99 era).
    * Action: Advance Global Epoch to $G=101$.

3.  **Reclamation Moment**:
    * Now $G=101$. This implies **every** active thread has observed at least era $100$.
    * Deduction: Since everyone is in era $100$ or $101$, **absolutely no one** can possibly hold a pointer to something from era $98$ ($G-2$).
    * **Conclusion: The $G=98$ trash can can safely be emptied.**

---

## 4. Code Anatomy

Refer to `include/ebr.h` as we break down the implementation.

### 4.1 Reader Side: High-Speed Entry
When a reader enters a critical section (e.g., `StealQueue::pop`), it performs two steps:

```cpp
void enter(LocalState* local) {
    // 1. Snapshot the global time
    // Relaxed is fine; slightly old time just keeps garbage longer
    size_t g = global_epoch_.load(std::memory_order_relaxed);
    local->epoch.store(g, std::memory_order_relaxed);
    
    // 2. Raise hand: "I'm inside!"
    // ⚠️ MUST be SeqCst (Sequential Consistency)
    local->active.store(true, std::memory_order_seq_cst); 
}
```

**🤔 Deep Dive: Why `seq_cst`?**
This prevents **Store-Load Reordering**.
* **Desired**: Store `active=true` -> Load `data_ptr`.
* **CPU Optimization**: Load `data_ptr` -> Store `active=true`.
* **Disaster**: If the load happens before the world sees the thread is active, a GC thread might delete the memory while the reader is accessing it.

### 4.2 Writer Side: Aggressive Advancement
When a trash can fills up, the writer attempts to advance the era.



```cpp
void try_advance() {
    size_t global = global_epoch_.load(std::memory_order_acquire);

    // 1. Global Scan
    for (auto& t : threads_) {
        // If someone is active and stuck in an old era...
        if (t->active && t->epoch != global) {
            return; // ⛔️ Someone is lagging. Cannot advance!
        }
    }

    // 2. Advance Epoch
    // Only happens when everyone has caught up to 'global'
    global_epoch_.store(global + 1, std::memory_order_release);

    // 3. Execute Reclamation
    // Clear the (Global - 2) bin.
    size_t safe_bin = (global + 2) % 3; // Mathematical magic for G-2
    clean_bin(safe_bin);
}
```

---

## 5. Comparison: EBR vs. Others

| Feature | `std::shared_ptr` | Hazard Pointers (HP) | EBR (This Solution) |
| :--- | :--- | :--- | :--- |
| **Reader Overhead** | **Very High** (Atomic ref-count) | **High** (Write barrier/list traversal) | **Ultra Low** (Local store) |
| **Writer Overhead** | Low | Medium | Medium |
| **Memory Footprint** | Low | Low | **Medium** (Cached garbage) |
| **Best Use Case** | Balanced Read/Write | Real-time / Low latency | **High Concurrency / Read-Heavy (Throughput King)** |

---

## 6. Pitfalls to Avoid

If you use EBR in your projects, follow these two **Golden Rules**:

1.  **Never Sleep While Active**:
    * If you call `sleep()` or `park()` after `enter()`, you become the "lagging" thread.
    * The Global Epoch will never advance, and memory will leak until the system crashes (OOM).
    * **Fix**: Always `exit()` before `park()`.

2.  **Unregister Dying Threads**:
    * When a thread exits, its `LocalState` must be removed from the global list. Otherwise, the GC will wait forever for a "dead" thread to catch up.
    * *(Note: The current implementation of `tiny_coro` simplifies this step for educational purposes, but `unregister_thread` must be implemented in a production environment).*