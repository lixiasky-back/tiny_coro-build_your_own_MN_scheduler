# Documentation: include/poller.h

## 1. 📄 Overview
**Role**: **The System's "Listen-In" (The I/O Event Listener)**.

In high-performance servers, we cannot afford to open a dedicated thread for every connection (the C10K problem).
The `Poller` leverages the most efficient mechanisms provided by the operating system (`epoll` on Linux, `kqueue` on macOS) to monitor thousands of sockets simultaneously.
* **Wait**: When all sockets are idle, the thread is suspended to save power.
* **Wake**: When data arrives (or a new task is added), the thread is instantly awakened.

It is the core driving engine of the **Reactor Pattern**.

---

## 2. 🏗️ Deep Dive

This file utilizes **Compile-time Polymorphism** via `#ifdef` macros to compile different implementations for different systems, avoiding the runtime overhead of virtual functions.

### 2.1 Linux Implementation (`epoll`)
`epoll` is the high-performance standard under Linux.



* **`eventfd` (Wake-up Mechanism)**:
    ```cpp
    wake_fd_ = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ```
    * **The Problem**: If `epoll_wait` is blocking while waiting for network data, but the main thread suddenly pushes a new task into the queue, how do we make `epoll_wait` return immediately to process it?
    * **The Solution**: Create a special "Wake-up File Descriptor." When a wake-up is needed, write 8 bytes (`uint64_t val = 1`) into it. `epoll` will detect a "readable event" and return instantly.

* **`EPOLLONESHOT` (The Critical Flag)**:
    ```cpp
    ev.events = EPOLLIN | EPOLLONESHOT | EPOLLRDHUP;
    ```
    * **Meaning**: After an event is triggered once, it is automatically disabled in `epoll` until you manually re-enable it.
    * **Why?** For multi-threaded safety. Without this, if data arrives continuously, Thread A might be woken up to read a socket, and `epoll` might trigger again immediately, causing Thread B to be woken up for the *same* socket. `ONESHOT` ensures only one thread processes a given socket at any time.

### 2.2 macOS Implementation (`kqueue`)
BSD-based systems (macOS/FreeBSD) use `kqueue`.

* **`EVFILT_USER` (Wake-up Mechanism)**:
    ```cpp
    EV_SET(&ev, wakeup_id_, EVFILT_USER, ...);
    ```
    * `kqueue` is arguably more elegant than `epoll` because it supports **user-defined events**. It doesn't require creating a real file descriptor (`eventfd`); you simply trigger a logical event to wake the poller.

* **`EV_ONESHOT`**:
  The equivalent of Linux's `EPOLLONESHOT`. The event is disabled after being triggered once.

---

## 3. 🎓 Technical Spotlight: I/O Multiplexing



### Plain English: What is a Poller?

* **Blocking I/O (Without Poller)**:
  Imagine a waiter (thread) who can only watch one table (socket).
    * If the customer doesn't order, the waiter just stands there (blocks).
    * To serve 1,000 tables, you'd need 1,000 waiters. This is too expensive (memory and context-switch overhead).

* **I/O Multiplexing (With Poller)**:
  Imagine a **Manager** (Poller) watching 1,000 tables.
    * When a customer at any table raises their hand (socket becomes readable), the Manager calls a waiter (worker coroutine) to handle it.
    * When nothing is happening, the Manager naps (`wait` blocking).
    * **Wakeup**: The kitchen (main thread) rings a bell. The Manager wakes up instantly, realizes no one raised a hand but there’s a new dish to be served (new task), and assigns someone to deliver it.

---

## 4. 💡 Design Rationale

### 4.1 Why is `udata` designed as `void*`?
```cpp
void add_read(int fd, void* udata);
template<typename F> int wait(..., F&& callback);
```
* **Decoupling**: The Poller doesn't need to know if it's storing a `Task`, a `Connection`, or an `HttpRequest`. It only cares about storing a pointer.
* **Flexibility**: When an event triggers, it returns this pointer verbatim to the callback. In your framework, `udata` is typically a coroutine handle (`coroutine_handle`) or an object wrapping it.

### 4.2 Why is the Wake mechanism necessary?
In `queue.h` and `worker.h`, a `wake` is called whenever a task is pushed.
* **Scenario**:
    1.  A Worker thread finishes all tasks and all sockets are idle.
    2.  The Worker calls `poller.wait()` and goes to sleep to save CPU.
    3.  The main thread `accept`s a new connection and places it in the Worker's queue.
    4.  **Without Wake**: The Worker would sleep until some *old* socket receives data. The new connection would "starve."
    5.  **With Wake**: The main thread writes to the `eventfd`, the Worker wakes up immediately, finds the new work, and gets to it.

### 4.3 Performance Details
* **`events_[128]`**: Allocating the event array on the stack avoids dynamic memory allocation (`new`/`malloc`) and improves cache locality.
* **`epoll_create1(EPOLL_CLOEXEC)`**: This prevents file descriptors from leaking to child processes if the process `fork`s—a standard practice for robust Linux service programming.