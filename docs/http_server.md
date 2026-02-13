# Documentation: include/http/http_server.h

## 1. 📄 Overview
**Role**: **The Application Layer (HTTP Business Logic)**.

`HttpServer` serves as the bridge between the low-level network I/O (`AsyncSocket`) and the high-level business logic.
* It doesn't care how the socket was connected (that’s the `Acceptor`’s job).
* It doesn't care how bytes are transmitted over the wire (that’s the `Reactor`’s job).
* **It only cares about**: How to package and send HTTP responses and how to stream incoming data into files.

It is the culmination of the **"Zero-Copy"** and **"Streaming"** design philosophies.

---

## 2. 🏗️ Deep Dive

### 2.1 `send_response`: Efficient Response Dispatch
```cpp
Task send_response(int code, std::string_view content_type, std::string_view body) {
    std::string res;
    // 1. Pre-allocate memory: prevents multiple reallocations during string concatenation
    res.reserve(256); 

    // 2. Concatenate HTTP headers
    res += "HTTP/1.1 " + std::to_string(code) ...;
    // ...

    // 3. Send Headers
    co_await socket_.write(res);
    
    // 4. Send Body - Zero-copy transmission
    if (!body.empty()) {
        co_await socket_.write(body.data(), body.size());
    }
}
```
* **Optimization Details**:
    * Headers and the Body are sent separately.
    * Headers are concatenated into a single `std::string` for a consolidated write.
    * The Body is sent directly via a pointer (`string_view`).
    * **Significance**: If the Body is a 10MB string, there is **no need** to copy it into the `res` string. The original memory address is handed directly to the socket. This is a massive memory optimization.

### 2.2 `receive_to_file`: Coroutine Streaming Upload
This is the **highlight** of this file, demonstrating how to implement a high-performance file upload server with just a few lines of code.



```cpp
Task receive_to_file(std::string_view save_path, size_t content_length, std::string_view initial_data) {
    std::ofstream file(path, binary);
    size_t total_received = 0;

    // 1. Handle "Pre-read" data (Initial Data)
    if (!initial_data.empty()) {
        file.write(initial_data.data(), ...);
        total_received += ...;
    }

    // 2. Streaming Loop
    char buf[8192]; // Fixed 8KB stack memory
    while (total_received < content_length) {
        // Calculate remaining bytes to read
        size_t to_read = std::min(sizeof(buf), content_length - total_received);
        
        // Key: co_await read
        // This suspends the coroutine while waiting for client data.
        ssize_t n = co_await socket_.read(buf, to_read);

        if (n <= 0) break; // Connection closed

        // Write to disk
        file.write(buf, n);
        total_received += n;
    }
}
```
* **Traditional Approach**: `malloc` a buffer as large as the file, read the entire Body into it, and then write to disk. If the file is 1GB, the server risks running out of memory (OOM).
* **Your Approach**: Whether the file is 1MB or 100GB, it only uses **8KB** of memory. Read a bit, write a bit.
* **Coroutine Magic**: While the `while` loop looks like a blocking loop, every `co_await read` yields the CPU back to the scheduler.

---

## 3. 🎓 Coroutine Spotlight

### Scenario: Why is `initial_data` critical?
In HTTP parsing (`http_parser.h`), when you call `read` to fetch Headers, TCP is a stream with no inherent boundaries.
* You might read 1000 bytes in one go.
* 200 bytes are the Headers.
* The remaining 800 bytes are actually the start of the Body.
* After the parser processes the Headers, these 800 bytes **must not be lost**! They must be passed to `receive_to_file` as `initial_data` to be written first before reading the rest of the stream.

### Scenario: Synchronous Semantics, Asynchronous Execution
In the `while` loop of `receive_to_file`:
* **The Programmer**: Writes code that feels like a simple, single-threaded blocking program with clear logic and no "Callback Hell."
* **The CPU**: While waiting for the network packet during `read`, it switches to execute tens of thousands of other requests. Although disk writing (`ofstream`) is technically blocking, OS Page Caching usually makes it extremely fast, preventing significant thread stalls.

---

## 4. 💡 Design Rationale

### 4.1 Why pass the Body via `std::string_view`?
* **Zero-Copy**: If you have `mmap`-ed a file into memory or the Body is a static string constant, passing a `string_view` requires no memory allocation.
* If you used `const std::string&`, the caller would be forced to construct a `string` object, which limits flexibility and performance.

### 4.2 Why a Buffer size of 8192?
* **Stack Constraints**: While coroutine frames are heap-allocated, keeping them small is beneficial.
* **Kernel Paging**: Linux default page size is typically 4KB. 8KB (two pages) is a classic I/O buffer size that balances reducing system call overhead with minimizing cache pressure.

### 4.3 Why does `send_response` return `Task`?
* Even though the function doesn't return a value to the user, it uses `co_await` internally, so it must be a coroutine.
* In C++20, coroutine functions must return a type compatible with `promise_type` (your `Task`).
* Callers need to `co_await` this Task to ensure the response is fully dispatched before closing the connection or proceeding to the next request.