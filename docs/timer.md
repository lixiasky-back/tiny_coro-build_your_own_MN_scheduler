# Documentation: include/timer.h

## 1. 📄 Overview
**Role**: **The Time Keeper**.

This file defines the basic unit for how the scheduler handles time-based events.
* **`TimePoint`**: Determines which "clock" we use to measure time.
* **`Timer`**: Defines what a timed task looks like (When does it go off? Who does it wake up?).

It is the fundamental element of the Min-Heap within the `Reactor`.

---

## 2. 🏗️ Deep Dive

### 2.1 `TimePoint`: Why use `steady_clock`?
```cpp
using TimePoint = std::chrono::steady_clock::time_point;
```
In C++, there are primarily two types of clocks:
1.  **`system_clock` (Wall Clock)**: Shows the current time and date. **It is mutable**. If you manually change the system time or if the Network Time Protocol (NTP) auto-calibrates, this clock will "jump."
2.  **`steady_clock` (Monotonic Clock)**: Like a stopwatch. It doesn't care about the actual time of day; it only cares about **how much time has passed since boot-up**. **It is monotonically increasing** and never goes backward.

* **Design Rationale**: Schedulers must use `steady_clock`.
    * Imagine you tell a coroutine to `sleep(10s)`.
    * If using `system_clock` and the system time happens to be rolled back by 1 hour, your coroutine might sleep for 1 hour and 10 seconds!
    * Using `steady_clock` ensures that regardless of system time changes, 10 seconds remains 10 seconds.

### 2.2 `struct Timer`: The Timer Carrier
```cpp
struct Timer {
    TimePoint expiry;               // Expiration time (Absolute time point)
    std::coroutine_handle<> handle; // Coroutine to resume when the alarm goes off
    // ...
};
```
* **`expiry`**: This is absolute time (e.g., the 10,000th second since boot), not relative time (e.g., "in 5 seconds"). The Reactor only needs to continuously check if `now() >= expiry`.
* **`handle`**: This uses `std::coroutine_handle<>` (type-erased handle).
    * This is **Type Erasure**. The Timer doesn't care if the coroutine is a `Task` or a `Generator`; it simply stores a `void*` pointer and calls `resume()` when the time comes.

### 2.3 `operator>`: The Key to Building a Min-Heap
```cpp
bool operator>(const Timer& other) const {
    return expiry > other.expiry;
}
```
This is designed for use with `std::priority_queue`.



* **Background Knowledge**:
    * `std::priority_queue` defaults to a **Max-Heap**, where the "largest" element is at the top (`top()`).
    * We want the **earliest expiring** timer at the top (Min-Heap).
* **Implementation Logic**:
    * When defining the heap in the `Reactor`, we use `std::greater<Timer>`.
    * `std::greater` internally calls the `operator>` we defined.
    * By providing the "greater than" logic to a Max-Heap structure (via `std::greater`), "later times" are treated as "larger" and sink to the bottom, while "earlier times" are treated as "smaller" and float to the top.
    * Final Result: `timers_.top()` always returns the timer with the smallest (earliest) `expiry`.

---

## 3. 🎓 C++20 and STL Spotlight

### `std::coroutine_handle<>` (Void Handle)
* **Layman's Explanation**: This is a "Universal Remote."
* A specific remote (`coroutine_handle<Promise>`) can only control a specific model of TV (a coroutine with a specific Promise type).
* A universal remote (`coroutine_handle<>`) can control any TV, but its functions are limited (it can only `resume` or `destroy`; it cannot access data inside the `Promise`).
* **Role in Timer**: The `Timer` doesn't need to know the return value of the coroutine; it just needs to press the "Play" button (`resume`) when the time is up.

---

## 4. 💡 Design Rationale

### 4.1 Why not use `std::function` callbacks?
Some timer implementations store a `std::function<void()>`.
* **Disadvantage**: `std::function` has memory allocation overhead (if the closure captures data) and is relatively bulky (usually 32 bytes or more).
* **Advantage**: `coroutine_handle<>` is essentially a **raw pointer (void*)**. It is tiny (8 bytes), and the copy overhead is zero. This is extremely memory-friendly for high-concurrency scenarios involving tens of thousands of timers.

### 4.2 Why put the comparison logic inside the struct?
* This is an **Intrusive** design. While we could write a lambda comparator in the `priority_queue` template parameters, overloading `operator>` is the standard C++ idiom. It gives the `Timer` object inherent "comparable" semantics.