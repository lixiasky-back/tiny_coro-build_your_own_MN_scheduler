# 🔍 Deep Dive: The Chase-Lev Lock-Free Work-Stealing Algorithm

> **Foreword**:
> In `tiny_coro`'s `StealQueue`, we have implemented a lock-free double-ended queue (Deque).
> This is no ordinary queue; it utilizes the classic algorithm proposed by Chase and Lev in 2005. Its core design philosophy is: **Push all synchronization costs onto the Thief, allowing the Owner to enjoy ultimate local performance.**

---

## 1. Core Architecture: The Bookshelf Model

Imagine a vertical bookshelf (circular array).
* **Bottom**: Accessible only by the **Owner**. The owner places books here (Push) and takes them from here (Pop).
* **Top**: Accessible only by the **Thief**, who can only steal books from this end.



### Key Variables
```cpp
alignas(64) std::atomic<long> top;    // Head index (contended by everyone)
alignas(64) std::atomic<long> bottom; // Tail index (modified only by owner, read by thieves)
std::atomic<Array*> array;            // Circular buffer
```
* **Top is monotonically increasing**: Even when the queue is empty, `top` does not retreat; it continues to grow.
* **Bottom is also monotonically increasing**: It only resets when a Pop fails (queue empty).

---

## 2. Roles & Operations

### 2.1 Owner Stocking: Push (Owner Only)
This is the fastest operation. Since only the Owner modifies `bottom`, **no CAS (Compare-And-Swap) is required** here—only a lightweight Store operation.

**Workflow**:
1.  Read `bottom` and `top`.
2.  **Check for expansion**: If `bottom - top >= capacity`, the queue is full and requires a `Resize`.
3.  **Write data**: Place the task into `array[bottom % cap]`.
4.  **Publish**: Increment `bottom++`. This requires a `Release` barrier to ensure task data is visible to others before the index updates.

**Performance**: Nanosecond scale. Nearly as fast as a standard array assignment.

### 2.2 The Thief's Heist: Steal (Thief Only)
The thief faces intense competition:
1.  Other thieves might be stealing simultaneously (multi-threaded contention for `top`).
2.  The owner might be Popping the very last element (contention with the Owner).

**Workflow**:
1.  Read `top` (Acquire).
2.  Read `bottom` (Acquire).
3.  Calculate task count: `size = bottom - top`.
4.  If `size <= 0`: Empty; nothing to steal, return failure.
5.  Read the task at `array[top % cap]`.
6.  **The Decisive Moment (CAS)**:
    * Attempt to change `top` from `t` to `t + 1`.
    * `compare_exchange_strong(top, t, t + 1)`.
    * **Success**: You stole it!
    * **Failure**: Another thief beat you to it, or the owner just Popped it. Retry or give up.

---

## 3. The Ultimate Challenge: Owner Pop Race

This is the most mind-bending part of the Chase-Lev algorithm.
The Owner takes tasks from the Bottom. Usually, this doesn't require a lock—unless **only one task remains** in the queue.

At this moment, the Owner reaches for the last task (Bottom end) while a Thief reaches for that same task (Top end). Without control, both would take the task, leading to **Double Consumption (Double Free)**.

### 3.1 Optimistic Execution Strategy
Chase-Lev takes a bold "act first, ask later" approach:

```cpp
std::optional<T> pop() {
    // 1. Pre-decrement Bottom: Regardless of thieves, I claim this territory first
    long b = bottom.load(relaxed) - 1;
    bottom.store(b, relaxed); 

    // 2. 🚧 Strong Fence (SeqCst) 🚧
    std::atomic_thread_fence(std::memory_order_seq_cst);

    // 3. Look back at Top: Check if anyone is currently stealing
    long t = top.load(seq_cst);

    // Case A: Plenty of tasks (t <= b)
    // Even after my decrement, top is still less than bottom. 
    // At least one task is safe. Return array[b] without CAS.

    // Case B: Queue is empty (t > b)
    // The queue was already empty; I decremented too far.
    // Restore bottom = b + 1 and return empty.

    // Case C: Conflict! (t == b)
    // There was exactly 1 task left. 
    // After decrementing bottom, top has caught up. 
    // I am now dueling with a thief for the sole remaining task.
    if (t == b) {
        // Duel! Attempt to increment top, representing "consuming this task"
        if (top.compare_exchange_strong(t, t + 1, ...)) {
            // I won! I pushed Top away; the thief's CAS will fail.
            bottom.store(b + 1, relaxed); // Restore bottom
            return task;
        } else {
            // I lost! A thief just incremented top. The task is gone.
            bottom.store(b + 1, relaxed); // Restore bottom
            return std::nullopt;
        }
    }
}
```

---

## 4. Deep Dive: Why must it be `SeqCst`?

In the `pop` function, this line is worth its weight in gold:
```cpp
std::atomic_thread_fence(std::memory_order_seq_cst);
```

This corresponds to the logic of the famous **Dekker's Algorithm** for mutual exclusion. We must guarantee that two operations are never reordered:
1.  **I modified Bottom** (`store b`).
2.  **I read Top** (`load t`).

**Without `SeqCst` (using only Acquire/Release)**:
* Modern CPUs (especially non-x86 architectures like ARM) allow **Store-Load reordering**.
* The CPU might execute "Read Top" first, see `t < b` (thinking it's safe), and only then execute "Modify Bottom."
* **Disaster Scenario**:
    1.  Queue has 1 element.
    2.  Owner reads Top, sees a task, prepares to take it.
    3.  Thief reads Bottom (old value), sees a task, CAS Top successfully, and steals it.
    4.  Owner finally modifies Bottom.
    5.  **Result**: Both Owner and Thief acquire the same pointer. The program logic collapses.

`SeqCst` forces the CPU to flush the Store Buffer, ensuring "my modification must be seen by the world before I can look at the world."



---

## 5. Resizing & Memory Reclamation (EBR)

When the queue is full, Chase-Lev needs to expand.
1.  Owner creates a new array, twice the size.
2.  Moves tasks over.
3.  Points the `array` pointer to the new array.

**The Problem**: A Thief might be paused on a pointer to the old array, about to read data. If the Owner simply `delete old_array`, the Thief crashes.
**The Solution**: This is why we use `EbrManager::retire(old_array)` in `include/queue.h`. We place the old array into a deferred reclamation station, only releasing it safely once the lagging Thief has left the critical section.

---

## 6. Summary

The Chase-Lev algorithm is a masterpiece of lock-free programming:
* **Extremely Owner-friendly**: Push and 99% of Pops require no heavy atomic operations (CAS), fully utilizing the CPU's local cache.
* **Fair to Thieves**: All thieves compete via atomic operations, and random stealing reduces collisions.
* **Minimalist Conflict Resolution**: Uses optimistic subtraction of `bottom` and a `seq_cst` fence to elegantly resolve contention for the final element.

By understanding this algorithm, you understand the secret behind the efficiency of Go and Rust schedulers.