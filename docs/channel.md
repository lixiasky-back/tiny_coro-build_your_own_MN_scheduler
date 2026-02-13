# Documentation: include/channel.h

## 1. 📄 Overview
**Role**: **The High-Speed Conveyor Belt for Coroutines**.

`Channel` is a pivotal component in the `tiny_coro` framework, implementing the **CSP (Communicating Sequential Processes)** model. It enables coroutines to synchronize and communicate by passing messages rather than through the perils of shared memory.

**Key Optimization in this Version**:
Implementation of the **`bool await_suspend`** strategy.
* **Traditional Approach**: `void await_suspend` -> Mandatory suspension -> Push to queue -> Scheduler wakes it up later.
* **Current Approach**: `bool await_suspend` -> Attempt to complete the operation within the lock -> **If successful, return false (do not suspend)** -> Continue execution immediately.
  This significantly reduces unnecessary context switches and scheduler overhead.

---

## 2. 🏗️ Deep Dive

### 2.1 Smart Suspension Mechanism (`bool await_suspend`)
This is the core of the performance boost. We no longer blindly suspend; instead, we follow the principle of "avoiding suspension whenever possible."



#### Send Logic (`SendAwaiter`)
```cpp
bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(chan.mtx_);

    // 1. Direct Handoff (The Express Path)
    // If someone is waiting to receive, hand the data directly to them. 
    // Not only do we skip the buffer, but we also avoid suspending ourselves!
    if (!chan.recv_waiters_.empty()) {
        auto& waiter = chan.recv_waiters_.front();
        *waiter.result_ptr = std::move(value); // Cross-stack write
        
        chan.sched_.spawn(Task::from_address(waiter.handle.address())); // Wake the receiver
        chan.recv_waiters_.pop();
        
        return false; // ✅ Critical: returns false, current coroutine resumes immediately!
    }

    // 2. Buffer Write (The High-Speed Path)
    if (chan.buffer_.size() < chan.capacity_) {
        chan.buffer_.push(std::move(value));
        return false; // ✅ Critical: Write successful, no suspension!
    }

    // 3. Blocked Suspension (The Slow Path)
    // No choice—no receiver available and buffer is full. Suspend and wait.
    chan.send_waiters_.push({h, &value});
    return true; // ⛔️ Returns true, control is handed back to the scheduler.
}
```

#### Receive Logic (`RecvAwaiter`)
```cpp
bool await_suspend(std::coroutine_handle<> h) {
    std::lock_guard<std::mutex> lock(chan.mtx_);

    // 1. Priority: Read from Buffer
    if (!chan.buffer_.empty()) {
        result = std::move(chan.buffer_.front());
        chan.buffer_.pop();

        // Chain Reaction: Since a slot opened up, pull a queued sender in to fill it.
        if (!chan.send_waiters_.empty()) {
            auto& sender = chan.send_waiters_.front();
            chan.buffer_.push(std::move(*sender.value_ptr));
            chan.sched_.spawn(Task::from_address(sender.handle.address())); // Wake the sender
            chan.send_waiters_.pop();
        }
        return false; // ✅ Data obtained, no suspension!
    }

    // 2. Direct Handoff (For capacity=0 or empty buffer)
    if (!chan.send_waiters_.empty()) {
        // Snatch data directly from the sender's hand
        auto& sender = chan.send_waiters_.front();
        result = std::move(*sender.value_ptr);
        
        chan.sched_.spawn(Task::from_address(sender.handle.address()));
        chan.send_waiters_.pop();
        return false; // ✅ Data obtained, no suspension!
    }

    // 3. Truly no data, suspend and wait
    chan.recv_waiters_.push({h, &result});
    return true;
}
```

---

## 3. 🎓 Technical Spotlight: The "Try-Lock" Optimization

### Plain English: Why is it efficient if `await_ready` is always `false`?

You might ask: *"Doesn't `await_ready` returning `false` mean we always have to suspend?"*

In C++20, this is a common misconception. The flow works like this:
1.  **`await_ready` returns `false`**: The compiler calls `await_suspend`.
2.  **Enter `await_suspend`**: We acquire the lock.
3.  **The Decision**:
* **Case A (Smooth Sailing)**: Buffer isn't full. We push the data and `return false`.
    * **Result**: The coroutine **does not** actually suspend. The compiler-generated code treats the suspension as "cancelled" and jumps straight to `await_resume`. It's like going to a bank: you take a number (prepare to wait), but see an open window immediately (resource available), so the manager waves you through without you ever sitting down.
* **Case B (Blocked)**: Buffer is full. We store the handle in the queue and `return true`.
    * **Result**: The coroutine truly suspends, and the thread moves on to other tasks.

**Advantage**: We consolidate "state checking" and "suspension logic" within a single protected critical section. This avoids race conditions while leveraging the `bool` return to bypass unnecessary scheduling overhead.

---

## 4. 💡 Design Rationale

### 4.1 Why not use `Scheduler::spawn` to re-schedule ourselves?
In older versions, even if we didn't block, we called `spawn` to put ourselves back in the queue.
* **Old Way**: `buffer.push()` -> `spawn(self)` -> `return void` (suspend).
    * **Cost**: The current thread must switch tasks, and the scheduler picks this coroutine up later. This is a full Context Switch, flushing CPU caches (L1/L2 Cache miss).
* **New Way**: `buffer.push()` -> `return false` (cancel suspension).
    * **Benefit**: The current thread continues with the very next line of the coroutine. Data remains in CPU registers or cache "hot zones," resulting in extreme performance.

### 4.2 Why does `capacity=0` achieve synchronization?
When `capacity=0`:
* **Sender**: `buffer.size() < 0` is always false, skipping buffer logic to check `recv_waiters_`. If empty, it suspends.
* **Receiver**: `buffer.empty()` is always true, skipping buffer logic to check `send_waiters_`. If empty, it suspends.
* This forces a **Rendezvous**—both parties must meet to proceed.

### 4.3 Why the need for `std::optional`?
To handle `close()` gracefully.
* If the Channel is closed and the buffer is empty, a `recv` operation won't suspend; it returns `false` (cancelling suspension) and provides `std::nullopt` in `await_resume`.
* This allows users to write clean loops: `while (auto val = co_await ch.recv()) { ... }`, which terminate naturally when the channel closes.