# Documentation: include/worker.h

## 1. 📄 Overview
**Role**: **The Autonomous Execution Unit**.

The `Worker` class represents a **logical execution unit** within the system (typically bound to a single physical kernel thread). It serves as the bridge between the scheduler and the CPU cores.

Each `Worker` is highly autonomous:
1.  **Self-Sustaining**: Owns a private `StealQueue` (lock-free queue).
2.  **Private Dormitory**: Owns a private `Parker` (for hibernation/sleeping).
3.  **Access Badge**: Owns a private `EbrManager::LocalState` (for memory safety).
4.  **Adaptable**: Owns an independent random number generator (`rng_`) to randomly "steal" work from others when idle.

---

## 2. 🏗️ Deep Dive

### 2.1 Member Variables: Fully Equipped
```cpp
class Worker {
    size_t id_;                      // Worker ID (0, 1, 2, ...)
    Scheduler& scheduler_;           // The "Boss" (used to find other workers)
    EbrManager::LocalState* ebr_state_; // EBR State Block (Access Badge)
    std::unique_ptr<StealQueue<Task>> local_queue_; // Local Task Queue (Ration Bag)
    Parker parker_;                  // Suspend/Wake mechanism (Dormitory)
    std::mt19937 rng_;               // RNG (Determines who to steal from)
};
```
* **`local_queue_`**: Managed using `std::unique_ptr`.
    * **Ownership**: The Worker has exclusive ownership over the queue's memory.
    * **Operations**: The Worker operates on the tail (`push/pop`), while thieves operate on the head (`steal`).
* **`rng_`**: Uses the Mersenne Twister algorithm.
    * **⚠️ Critical Detail**: Must be **independently seeded** (e.g., using `id_` or `std::random_device`). Otherwise, all Workers generate identical random sequences, leading them to steal from the same victim simultaneously, causing unnecessary contention.

### 2.2 Core Methods
* **`run()`**:
  The main loop of the Worker. Logic must strictly follow this order to prevent deadlocks:
    1.  **Enter critical section (`enter`)**.
    2.  Attempt to find tasks in this order: Local Queue -> Global Queue -> Neighbor Queues (Steal).
    3.  If a task is found: Execute it.
    4.  If no task is found:
    * **Must exit critical section first (`exit`)**: Prevent sleeping in an `Active` state, which would block the global Epoch from advancing and stall memory reclamation (OOM risk).
    * Call `parker_.park()` to suspend the thread and wait for a wake-up call.

* **`schedule(Task t)`**:
    * **Fast Path**: When a Worker generates a new task (e.g., Coroutine A `spawns` Coroutine B), it pushes it directly into the local queue.
    * **Lock-free & Contention-free**: Since it pushes to the local tail (Bottom), only the owner operates here, making it extremely fast (nanosecond scale).

* **`steal()`**:
    * **Passive Interface**: Called by **other starving Workers**.
    * It invokes the underlying `StealQueue::steal()` to attempt to take a task from the head (Top).

---

## 3. 🎓 Core Strategy: Work Stealing

### 3.1 Why "Random Stealing"?



When searching for tasks, we use `rng_` to select a victim.
* **Scenario**: Imagine 10 checkout counters (Workers).
* **Round-Robin**: Cashier A is idle and always checks B, then C, then D...
    * **Flaw**: If everyone is idle, they all congest while checking B, leading to cache line contention.
* **Random**: Cashier A is idle, closes their eyes and points: "I'll help F!"
    * **Mathematical Proof**: Random victim selection balances load fastest, and the probability of conflict decreases exponentially as the number of Workers increases.

---

## 4. 💡 Design Rationale

### 4.1 Why hold a `LocalState*` instead of using `thread_local`?
* **Performance**: Accessing member variables via a pointer is generally faster than TLS (Thread Local Storage) addressing.
* **Dependency Injection**: `LocalState` must be passed to the `StealQueue` for resource reclamation; holding the pointer explicitly makes dependencies between modules more transparent.

### 4.2 Why disable copying?
```cpp
Worker(const Worker&) = delete;
```
* A Worker is one-to-one with a physical thread. Copying a Worker would result in two objects trying to control the same `Parker` (wake-up confusion) or the same `StealQueue` (violating single-producer semantics), which are fatal concurrency errors.

### 4.3 Why doesn't `Worker` contain a `std::thread` member?
* **Decoupling**: `Worker` defines *how* tasks are processed (logic), while the physical thread defines *how* resources are allocated.
* **Flexibility**: This allows the `Scheduler` to flexibly decide how to bind threads, such as implementing **Core Affinity**—binding specific Workers to specific physical cores to reduce L3 Cache jitter.