# tiny_coro Full Development Documentation & API Reference Manual

**tiny_coro** is a high-performance asynchronous runtime framework based on **C++20 Coroutines**. It utilizes an **M:N threading model** (mapping M coroutines to N kernel threads), combining a **Work-Stealing** scheduling algorithm with **EBR (Epoch-Based Reclamation)** memory management to deliver a lock-free, highly concurrent, and low-latency asynchronous programming experience.



---

## Table of Contents

1.  **Core Runtime & Scheduling**
    * Scheduler & Worker
    * Reactor & Poller
    * Task & Timer
    * Queues (StealQueue & GlobalQueue)
2.  **Network I/O**
    * TcpListener
    * AsyncSocket
3.  **HTTP Application Layer**
    * HttpParser
    * HttpServer (Response & Streaming)
4.  **Concurrency & Synchronization**
    * AsyncMutex
    * Channel (CSP-style)
    * SpinLock & Parker
5.  **Memory Management (Lock-Free Safety)**
    * EbrManager & EbrGuard
6.  **Full Practical Examples**

---

## 1. Core Runtime & Scheduling

### Headers
* `#include "scheduler.h"`
* `#include "task.h"`
* `#include "queue.h"`
* `#include "timer.h"`

### `class Scheduler`
The control center of the system. It manages the Worker thread pool, the I/O Reactor, and the global task queue.

| API Method | Description | Parameters / Return Value |
| :--- | :--- | :--- |
| **`Scheduler(size_t n)`** | **Constructor**. Initializes the runtime environment. | `n`: Number of worker threads (Default: CPU core count). |
| **`void spawn(Task t)`** | **Submit Task**. Pushes a coroutine task into the global queue and wakes a Worker. | `t`: `Task` object returned by a coroutine. Lifecycle is handled internally. |
| **`Reactor* reactor()`** | **Get Reactor**. Used to initialize network components. | Returns a pointer to the underlying `Reactor`. |
| **`size_t worker_count()`** | **Get Thread Count**. | Returns the number of active worker threads. |
| **`~Scheduler()`** | **Destructor**. | Sends stop signals, wakes all threads for reclamation, and exits safely. |

### `class Reactor`
The event-driven heart of the scheduler, running on its own thread. It handles both I/O multiplexing (via `epoll`/`kqueue`) and timer expirations (via a Min-Heap).



| API Method | Description | Parameters / Return Value |
| :--- | :--- | :--- |
| **`void add_timer(TimePoint expiry, handle)`** | **Register Timer**. | Registers a coroutine to be resumed at a specific `steady_clock` time. |
| **`void register_read(int fd, void* handle)`** | **Register I/O Read**. | Suspends coroutine until `fd` is readable. |
| **`void register_write(int fd, void* handle)`**| **Register I/O Write**. | Suspends coroutine until `fd` is writable. |

### `struct Task`
The standard return type for all asynchronous coroutine functions.
* **Features**: Implements the C++20 coroutine `promise_type`. Features **Symmetric Transfer** for stackless deep nesting.
* **Memory Safety**: Maintains internal atomic reference counting (`ref_count`).
* **Advanced Lifecycle API**:
    * `void* detach()`: Strips the coroutine handle ownership for raw pointer storage (used by lock-free queues).
    * `static Task from_address(void* ptr)`: Restores a `Task` object from a raw pointer.

### `async function sleep_for`
Asynchronous sleep function (Timer).

```cpp
// Prototype
AsyncSleep sleep_for(Scheduler& s, int ms);

// Usage
co_await sleep_for(sched, 1000); // Suspends current coroutine for 1s; thread picks up other tasks.
```

### `Queues (GlobalQueue & StealQueue)`
The underlying data structures driving the Work-Stealing model.
* **`GlobalQueue<T>`**: A thread-safe, mutex-protected `std::deque` for external task submissions.
* **`StealQueue<T>`**: A lock-free, SPMC (Single-Producer Multi-Consumer) queue based on the **Chase-Lev algorithm**. It uses `alignas(64)` to prevent false sharing and ensures zero-contention task execution for the queue owner.



---

## 2. Network I/O

### Headers
* `#include "socket.h"`

### `class TcpListener`
Used for server-side TCP connection listening. Automatically configures sockets to non-blocking mode.

| API Method | Description | Parameters / Return Value |
| :--- | :--- | :--- |
| **`TcpListener(Reactor* r)`** | **Constructor**. | `r`: Obtained via `sched.reactor()`. |
| **`int bind(const char* ip, int port)`** | **Bind Address**. Executes `socket`, `bind`, and `listen`. | `ip`: Listen IP (e.g., "0.0.0.0"). <br>`port`: Port number. <br>Returns: 0 on success, -1 on failure. |
| **`CoAccept accept()`** | **Accept Connection**. **Awaitable**. | **Usage**: `AsyncSocket client = co_await listener.accept();`<br>Returns: An established `AsyncSocket` object. |

### `class AsyncSocket`
A wrapper for asynchronous non-blocking TCP sockets. Follows strict RAII principles; the connection closes automatically upon destruction.

| API Method | Description | Parameters / Return Value |
| :--- | :--- | :--- |
| **`AsyncReadAwaiter read(void* buf, size_t size)`** | **Async Read**. **Awaitable**. | `buf`: Receive buffer. <br>`size`: Buffer size. <br>**Returns**: `ssize_t` (bytes read, 0 for closed, <0 for error). |
| **`AsyncWriteAwaiter write(const void* buf, size_t len)`** | **Async Write**. **Awaitable**. | `buf`: Pointer to data. <br>`len`: Data length. <br>**Returns**: `ssize_t` (bytes written). |
| **`write(const std::string& s)`** | **String Write Overload**. | Helper method to send a `std::string`. |
| **`int fd()`** | **Get Native FD**. | Used for low-level operations (e.g., `setsockopt`). |
| **`AsyncSocket(AsyncSocket&&)`** | **Move Constructor**. | Supports ownership transfer. **Copying is strictly disabled**. |

---

## 3. HTTP Application Layer

### Headers
* `#include "http_parser.h"`
* `#include "http_server.h"`

### `struct HttpRequest`
A lightweight, **Zero-Copy** view of an HTTP request.
* **Members**:
    * `std::string_view method`: HTTP Method ("GET", "POST").
    * `std::string_view path`: Request Path ("/index.html").
    * `std::vector<Header> headers`: List of headers.
* **Methods**:
    * `std::string_view get_header(name)`: Linear search for a header value.

### `class HttpParser`
A static utility class wrapping `picohttpparser` for high-speed parsing.

```cpp
// Parse Request
// Returns: >0 (Total Header length), -1 (Error), -2 (Incomplete data)
static int parse_request(const char* buf, size_t len, HttpRequest& req);
```

### `class HttpServer`
A helper class built on top of `AsyncSocket` for HTTP handling.

| API Method | Description | Parameters / Return Value |
| :--- | :--- | :--- |
| **`HttpServer(AsyncSocket& s)`** | **Constructor**. | Pass a reference to an active `AsyncSocket`. |
| **`Task send_response(int code, type, body)`** | **Send Response**. Auto-constructs Headers and Body. | `code`: HTTP Status (200, 404). <br>`type`: Content-Type. <br>`body`: Content string. |
| **`Task receive_to_file(path, len, init_data)`** | **Stream to File**. Writes to disk using a small fixed buffer (8KB). | `path`: Save path. <br>`len`: Content-Length. <br>`init_data`: Pre-read body data from the parsing phase. |

---

## 4. Concurrency & Synchronization

### Headers
* `#include "async_mutex.h"`
* `#include "channel.h"`
* `#include "spinlock.h"`
* `#include "parker.h"`

### `class AsyncMutex`
A cooperative mutex. It **suspends the coroutine** on contention instead of blocking the kernel thread, yielding the CPU to other coroutines immediately.

| API Method | Description | Usage Example |
| :--- | :--- | :--- |
| **`AsyncMutex(Scheduler& s)`** | **Constructor**. | Requires a Scheduler reference for waking. |
| **`LockAwaiter lock()`** | **Lock**. **Awaitable**. | `auto guard = co_await mutex.lock();`<br>Returns an RAII `ScopedLock` object. |
| **`void unlock()`** | **Unlock**. | Usually called automatically by `ScopedLock`. Uses Baton Passing. |

### `class Channel<T>`
CSP-style communication channel for safe data exchange between coroutines.

| API Method | Description | Behavioral Details |
| :--- | :--- | :--- |
| **`Channel(Scheduler& s, size_t cap)`** | **Constructor**. | `cap=0`: Unbuffered (Synchronous rendezvous). <br>`cap>0`: Buffered. |
| **`send(T val)`** | **Send**. **Awaitable**. | Suspends if the buffer is full (or no receiver in unbuffered mode). |
| **`recv()`** | **Receive**. **Awaitable**. | Suspends if the buffer is empty (or no sender in unbuffered mode). <br>Returns `std::optional<T>`. |
| **`void close()`** | **Close Channel**. | Wakes all waiters; `recv` will subsequently return `nullopt`. |

### Low-Level Sync: `SpinLock` & `Parker`
* **`SpinLock`**: A TTAS (Test-Test-And-Set) user-space spinlock optimized with `_mm_pause()`. Used internally for ultra-short critical sections (nanosecond duration) to prevent Cache Line Bouncing.
* **`Parker`**: A thread-suspension primitive built on `std::condition_variable` (acting as a `futex` wrapper), allowing Worker threads to sleep with zero CPU usage when no tasks are available.

---

## 5. Memory Management (Lock-Free Safety)

### Headers
* `#include "ebr.h"`

### `class EbrManager`
Implements **Epoch-Based Reclamation (EBR)** to solve the ABA problem and prevent premature memory deallocation in the lock-free `StealQueue`.



| API Method | Description | Note |
| :--- | :--- | :--- |
| **`static EbrManager& get()`** | **Singleton Access**. | Global epoch coordinator. |
| **`LocalState* register_thread()`** | **Thread Registration**. | Called once per Worker thread upon creation. |
| **`void retire(LocalState*, T* ptr)`** | **Deferred Deletion**. | Marks a pointer for deletion. It will only be `delete`d when no thread is observing the old epoch. |

* **`EbrGuard`**: An RAII helper used by Worker threads. Entering an `EbrGuard` scope marks the thread as `Active` in the current epoch, ensuring safe reads from the lock-free queues.

---

## 6. Full Practical Examples

### Example 1: Hello World (Basic Scheduling)
```cpp
#include "scheduler.h"
#include <iostream>

Task hello(Scheduler& sched) {
    std::cout << "[Step 1] Hello from coroutine!\n";
    // Simulate an async operation without blocking the thread
    co_await sleep_for(sched, 1000); 
    std::cout << "[Step 2] World after 1 second!\n";
}

int main() {
    Scheduler sched; // Start runtime (uses hardware concurrency by default)
    sched.spawn(hello(sched));
    
    // Block main thread just to keep the demo alive
    std::this_thread::sleep_for(std::chrono::seconds(2));
    return 0;
}
```

### Example 2: Channel Producer-Consumer
```cpp
#include "scheduler.h"
#include "channel.h"
#include <iostream>

Task producer(Channel<int>& chan) {
    for (int i = 0; i < 5; ++i) {
        std::cout << "Producing " << i << "\n";
        co_await chan.send(i); // Suspends if queue is full
    }
    chan.close();
}

Task consumer(Channel<int>& chan) {
    while (true) {
        auto val = co_await chan.recv(); // Suspends if empty
        if (!val) break; // Channel closed
        std::cout << "Consumed " << *val << "\n";
    }
}

int main() {
    Scheduler sched;
    Channel<int> chan(sched, 2); // Buffer size of 2

    sched.spawn(producer(chan));
    sched.spawn(consumer(chan));

    std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}
```

### Example 3: High-Performance HTTP File Upload Server
A complete integration of the network, parser, and streaming components.

```cpp
#include "scheduler.h"
#include "socket.h"
#include "http_server.h"
#include <iostream>

// Handle single HTTP request
Task handle_http_client(AsyncSocket socket) {
    HttpServer server(socket);
    char buf[8192];
    
    // 1. Read Header
    ssize_t n = co_await socket.read(buf, sizeof(buf));
    if (n <= 0) co_return;

    HttpRequest req;
    // Zero-Copy Parser
    int prev_len = HttpParser::parse_request(buf, n, req);

    if (prev_len > 0) {
        // Route: POST /upload
        if (req.method == "POST" && req.path == "/upload") {
            // Get Content-Length
            std::string_view len_str = req.get_header("Content-Length");
            size_t content_len = std::stoul(std::string(len_str));

            // Calculate remaining body data already in buffer
            std::string_view initial_body(buf + prev_len, n - prev_len);

            // 2. Stream receive to disk (Memory efficient)
            std::cout << "Receiving file, size: " << content_len << "\n";
            co_await server.receive_to_file("uploaded_file.bin", content_len, initial_body);

            // 3. Send Response
            co_await server.send_response(200, "text/plain", "Upload Success!");
        } else {
            co_await server.send_response(200, "text/html", "<h1>Hello tiny_coro!</h1>");
        }
    }
}

// Listening Coroutine
Task start_server(Scheduler& sched, int port) {
    TcpListener listener(sched.reactor());
    if (listener.bind("0.0.0.0", port) < 0) {
        std::cerr << "Bind failed\n";
        co_return;
    }
    std::cout << "Server running on [http://127.0.0.1](http://127.0.0.1):" << port << "\n";

    while (true) {
        // Async Accept
        AsyncSocket client = co_await listener.accept();
        // Spawn new coroutine; transfer ownership
        sched.spawn(handle_http_client(std::move(client)));
    }
}

int main() {
    Scheduler sched(4); // 4 Worker Threads
    sched.spawn(start_server(sched, 8080));

    // Keep main thread alive
    while(true) std::this_thread::sleep_for(std::chrono::seconds(1));
    return 0;
}
```

---