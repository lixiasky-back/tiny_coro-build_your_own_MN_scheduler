# 🏛️ Deep Dive: Dekker's Mutual Exclusion Algorithm (The First Solution)

> **Foreword**:
> In an era without hardware aids like CAS (Compare-and-Swap) or Test-and-Set, how do you keep two threads from colliding?
> Dekker's algorithm answers with: **Politeness and Persistence**.
> it solves not just mutual exclusion, but also avoids deadlocks (stuck forever) and starvation (never getting a turn).

---

## 1. The Primitives

Assume we have two threads: $T_0$ and $T_1$. We need three shared variables:

1.  **Intent Flags (`wants_to_enter[2]`)**:
    * `wants_to_enter[0] = true` means $T_0$ wants to enter the critical section.
    * `wants_to_enter[1] = true` means $T_1$ wants to enter the critical section.
2.  **Turn Token (`turn`)**:
    * `turn = 0` indicates that if a conflict occurs, $T_0$ has priority.
    * `turn = 1` indicates that $T_1$ has priority.

---

## 2. The Protocol

The essence of Dekker's algorithm is **"yielding."** The logic is as follows:

1.  **Raise Hand**: I want to enter (`wants_to_enter[me] = true`).
2.  **Check Opponent**: Does the other thread want to enter too? (`wants_to_enter[other] == true`?)
    * **If No**: Great, enter the critical section immediately.
    * **If Yes**: A conflict has occurred. We must check the `turn` token.
        * If `turn` is **Me**: I stay firm and wait for the other to give up.
        * If `turn` is **The Other**: **I lower my hand** (`wants_to_enter[me] = false`) to let them through.
        * I wait for the `turn` to become mine.
        * Once the `turn` is mine, I **raise my hand again** and retry.
3.  **Exit**: Work is done. Hand the `turn` to the other thread and lower my hand.



---

## 3. C++ Implementation (Modern Interpretation)

To function correctly on modern CPU architectures, we must use `std::atomic` and `seq_cst` (Sequential Consistency).

```cpp
#include <atomic>
#include <thread>

class DekkerLock {
    std::atomic<bool> wants_to_enter[2];
    std::atomic<int> turn;

public:
    DekkerLock() {
        wants_to_enter[0] = false;
        wants_to_enter[1] = false;
        turn = 0;
    }

    void lock(int me) {
        int other = 1 - me;
        
        // 1. Raise hand (Store)
        wants_to_enter[me].store(true, std::memory_order_seq_cst);

        // 2. Check opponent (Load)
        while (wants_to_enter[other].load(std::memory_order_seq_cst)) {
            // Conflict! Check if it is my turn
            if (turn.load(std::memory_order_relaxed) != me) {
                // Not my turn, I yield first
                wants_to_enter[me].store(false, std::memory_order_relaxed);
                
                // Wait for the turn to become mine (Spin)
                while (turn.load(std::memory_order_relaxed) != me) {
                    // CPU pause for optimization
                }
                
                // My turn, raise hand again
                wants_to_enter[me].store(true, std::memory_order_seq_cst);
            }
        }
    }

    void unlock(int me) {
        // 3. Hand over the token and lower hand
        int other = 1 - me;
        turn.store(other, std::memory_order_release);
        wants_to_enter[me].store(false, std::memory_order_release);
    }
};
```

---

## 4. Why is it a "Deadlock Killer"? (Analysis)

### 💀 Deadlock Scenario (Naive Attempt)
If you only used a "raise hand" mechanism:
1. $T_0$ raises hand.
2. $T_1$ raises hand.
3. $T_0$ sees $T_1$ has a hand up and waits.
4. $T_1$ sees $T_0$ has a hand up and waits.
5. **They stare at each other until the end of time.**

### ✅ Dekker's Solution
Introduce the `turn` to break the symmetry.
* When both raise hands, `turn` is guaranteed to be either 0 or 1.
* The thread owning the `turn` (e.g., $T_0$) stays persistent.
* The thread without the `turn` (e.g., $T_1$) will **lower its hand** and enter a waiting loop.
* Once $T_1$ lowers its hand, $T_0$'s `while` condition is no longer met, and $T_0$ enters the critical section.

---

## 5. The Modern Hardware Trap: Store Buffers

This is the most critical lesson Dekker's algorithm provides for modern programming.

### 🚨 Fatal Reordering
At the start of the `lock` function:
```cpp
wants_to_enter[me] = true;  // Store A
if (wants_to_enter[other])  // Load B
```
This is a classic **Store-Load** sequence.



* **In Theory**: I write A first, then read B.
* **In Reality (x86/ARM)**:
    1. The CPU writes `true` into a **Store Buffer**, which hasn't yet flushed to L1 cache or Main Memory.
    2. The CPU immediately executes Load B.
    3. At this moment, the other core cannot yet see that you raised your hand.
    4. **Result**: $T_0$ thinks $T_1$ hasn't raised a hand, and $T_1$ thinks $T_0$ hasn't either.
    5. **Boom! Both enter the critical section. Mutual exclusion fails.**

### 🛡️ The Solution: SeqCst
This is why `std::memory_order_seq_cst` must be used. It generates hardware instructions (like `mfence` on x86 or specific barriers on ARM) that force the CPU to **"flush the Store Buffer completely before performing the Load."**

This explains the critical line in the Chase-Lev algorithm (`queue.h`):
```cpp
// Chase-Lev Pop logic
bottom.store(b);
std::atomic_thread_fence(std::memory_order_seq_cst); // 👈 Preventing Store-Load reordering
long t = top.load();
```
Essentially, when Chase-Lev handles the competition for the "last element" in the queue, it is performing a simplified version of Dekker’s mutual exclusion.

---

## 6. Summary

Dekker's algorithm is a living fossil of concurrent programming, teaching us three things:
1.  **Break Symmetry**: Use a `turn` variable to resolve deadlocks.
2.  **Polite Yielding**: Temporarily giving up resources (lowering your hand) is the key to solving livelocks.
3.  **Memory Ordering**: On multi-core CPUs, **"I wrote it" does not mean "Others saw it."** Without a Memory Fence, even logically perfect algorithms will collapse.