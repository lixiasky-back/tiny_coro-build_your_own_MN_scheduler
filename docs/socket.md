# Documentation: include/socket.h

## 1. 📄 Overview
**Role**: **The Coroutine Adapter for Network I/O**.

`socket.h` is the frontline where the `tiny_coro` framework interacts with the operating system kernel. It is responsible for transforming traditional, procedural Linux Socket APIs (like `read`, `write`, `accept`) into C++20 **Awaitable objects**.

This adapter allows you to write high-performance network code that looks synchronous but is entirely asynchronous:
```cpp
// User code: As simple as writing synchronous code
AsyncSocket client = co_await listener.accept();
auto n = co_await client.read(buf, 1024);
co_await client.write(buf, n);
```

---

## 2. 🏗️ Deep Dive

### 2.1 Non-blocking Mode (`set_nonblocking`)
```cpp
inline void set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}
```
* **Core Mechanism**: This is the foundation of the Reactor pattern.
* **Function**: Sets the socket to non-blocking mode.
    * **Blocking Mode**: `read` puts the thread to sleep if no data is available.
    * **Non-blocking Mode**: `read` returns `-1` immediately and sets `errno = EAGAIN` if no data is present. This provides the coroutine the opportunity to "suspend and switch tasks."

### 2.2 Awaiters: The Three-Step Dance
All Awaiters (`AsyncReadAwaiter`, `AsyncWriteAwaiter`, `AsyncAcceptAwaiter`) follow the same logical pattern.

#### Step 1: `await_ready` (The Fast Path)
```cpp
bool await_ready() {
    result_ = ::read(fd_, buffer_, size_);
    // 1. If data is in the kernel buffer, read it directly; do not suspend (Fast Path)
    if (result_ >= 0) return true; 
    // 2. If data is temporarily unavailable, suspension is required
    if (errno == EAGAIN || errno == EWOULDBLOCK) return false;
    // 3. For real errors (e.g., connection reset), do not suspend; resume to handle the error
    return true;
}
```
* **Performance Key**: Under high load, data often resides in the kernel buffer already. `await_ready` avoids the expensive "Suspend -> Register with epoll -> Wake up" cycle, drastically reducing latency.

#### Step 2: `await_suspend` (Suspend & Register)
Executed when `await_ready` returns `false`.
```cpp
void await_suspend(std::coroutine_handle<Task::Promise> h) {
    // 1. Keep-alive via reference counting
    h.promise().ref_count.fetch_add(1, std::memory_order_seq_cst);
    
    // 2. Register event (Tell Reactor)
    reactor_->register_read(fd_, h.address());
}
```
* **Critical Fix for AsyncWriteAwaiter**:
  In `AsyncWriteAwaiter`, the code correctly invokes:
    ```cpp
    reactor_->register_write(fd_, h.address()); // ✅ Correct: Listen for writable events
    ```
  This prevents a **deadlock** that occurs when the send buffer is full and the system erroneously waits for a readable event.

#### Step 3: `await_resume` (Restoring Context)
Executed after the coroutine is awakened by the Reactor.
```cpp
ssize_t await_resume() {
    // Since we were woken by the Reactor, we should be able to read/write now
    if (result_ < 0 && (errno == EAGAIN ...)) {
        result_ = ::read(fd_, buffer_, size_);
    }
    return result_; // Return the final result
}
```

---

## 3. 🎓 Technical Spotlight: The Bank Teller Analogy



To explain how these hooks interact, imagine **visiting a bank teller**:

1.  **`await_ready` (Check the window before taking a ticket)**:
    * You walk up to the counter and look if a teller is free.
    * **Busy (EAGAIN)**: You need to take a ticket and wait (Return `false`, suspend).
    * **Free (Success)**: You sit down and complete your business immediately (Return `true`, no suspension).

2.  **`await_suspend` (Taking a ticket and waiting)**:
    * You take a ticket (`coroutine_handle`) and leave your phone number with the floor manager (`Reactor`).
    * **Ref Count**: You tell your family (Scheduler), "I'm at the bank, don't worry," so they don't think you're missing (Prevents Task destruction).
    * **Register Event**:
        * **Read**: "Call me when it's my turn."
        * **Write**: "Call me when the window opens up."

3.  **`await_resume` (Getting called)**:
    * The manager shouts: "Ticket 10086 to Window 1!"
    * You go to the window, finish your business (`read/write`), and leave with your receipt (Return value).

---

## 4. 💡 Design Rationale

### 4.1 Why must `AsyncWriteAwaiter` use `register_write`?
* **Why writes block**: The TCP Send Buffer is full. This happens when sending large files (images/video) over a restricted bandwidth.
* **The Wrong Way (register_read)**: You wait for "the client to send data." The client waits for "you to finish sending." Result: **Deadlock**.
* **The Right Way (register_write)**: You wait for "the kernel buffer to clear up." Once there is space, the Reactor notifies you to continue filling the buffer.

### 4.2 Why manually increment Ref Count in `await_suspend`?
* **Lifecycle Race**: The Reactor might trigger an event and execute the coroutine in a different thread almost instantly.
* If the reference count isn't incremented, the coroutine might finish and destroy the `Task` while `await_suspend` hasn't even returned in the original thread. This leads to **Use-After-Free** crashes. Incrementing by 1 tells the system: "The Reactor holds a reference now; do not delete yet."

### 4.3 Why is copying disabled for `AsyncSocket`?
* **Ownership Model**: A Socket is an exclusive resource. If copying were allowed, two `AsyncSocket` objects would hold the same file descriptor (fd). If one destructs and closes the fd, the other object would operate on a closed or—worse—a reused fd, leading to severe data corruption bugs.