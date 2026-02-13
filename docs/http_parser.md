# Documentation: include/http/http_parser.h

## 1. 📄 Overview
**Role**: **The Zero-Copy Translator**.

This file is responsible for instantaneously converting the raw binary stream (`const char* buf`) read from a socket into C++ objects (`HttpRequest`). Its core design philosophy is **"Look, don't touch"**: it never creates new `std::string` copies for HTTP Methods, Paths, or Headers. Instead, it makes the `std::string_view` members in the `HttpRequest` object point directly to the memory addresses within the original buffer.

This approach makes parsing over **10 times faster** than traditional methods using `std::stringstream` or `std::string` copies.

---

## 2. 🏗️ Deep Dive

### 2.1 Data Structure: `HttpRequest`
```cpp
struct HttpRequest {
    std::string_view method;
    std::string_view path;
    // ...
    struct Header {
        std::string_view name;
        std::string_view value;
    };
    std::vector<Header> headers;
    // ...
};
```
* **The Magic of `std::string_view`**:
    * Here, `method` and `path` do not own memory; they are simply "windows" into the original buffer.
    * **Lifecycle Warning**: Since these are views, the **original buffer must remain valid** while the `HttpRequest` is in use. In your framework, the buffer resides in the connection's memory, ensuring safety.

* **Linear Search in `get_header`**:
    * You use a simple `for` loop to traverse headers.
    * **Why not `std::map`?**
        * Building a `std::map` requires significant memory allocation (node creation) and hash calculations.
        * Most HTTP requests have only 5–10 headers. For such small datasets, a linear scan is faster and carries zero allocation overhead.

### 2.2 Parsing Logic: `HttpParser`
This is a C++ wrapper around `picohttpparser`.

```cpp
static int parse_request(const char* buf, size_t len, HttpRequest& req) {
    // 1. Stack-allocated Header array
    // Ultimate optimization: avoids malloc/new
    struct phr_header headers[32]; 
    size_t num_headers = 32;

    // 2. Call picohttpparser (C library)
    int ret = phr_parse_request(buf, len, &method_ptr, &method_len, ..., headers, &num_headers, 0);

    // 3. Result Processing
    if (ret > 0) {
        // Success: convert C pointers to C++ string_view
        req.method = {method_ptr, method_len};
        // ...
    }
    return ret;
}
```
* **Return Value Meanings**:
    * `> 0`: **Parsing Success**. Returns the total length of the HTTP header (Headers + blank line). You can use this length to advance the buffer pointer to the start of the Body.
    * `-1`: **Parsing Error**. The client sent invalid HTTP data.
    * `-2`: **Incomplete Data**. This is the most common state in non-blocking I/O. It means "some data read, but haven't reached the ending `\r\n\r\n` yet." The scheduler should continue waiting for `read` events.

---

## 3. 🎓 Technical Spotlight: Zero-Copy Parsing



### Plain English: How is Zero-Copy achieved?

* **Traditional Way (Copy)**:
    1. Socket reads `"GET /index.html"`.
    2. Program does `new string("GET")` -> Memory copy.
    3. Program does `new string("/index.html")` -> Memory copy.
    * **Disadvantage**: CPU is busy hauling data; high memory fragmentation.

* **Tinycoro's Way (View)**:
    1. Socket reads `"GET /index.html"` (assume address `0x1000`).
    2. `req.method` points to `0x1000` with length 3.
    3. `req.path` points to `0x1004` with length 11.
    * **Advantage**: No `new`, no `memcpy`. The CPU just records two pointers. This is a major reason why the system can achieve 180k+ QPS.

---

## 4. 💡 Design Rationale

### 4.1 Why choose `picohttpparser`?
* It is widely recognized as one of the fastest HTTP parsers (used in the H2O server and parts of Node.js).
* It utilizes **SIMD (SSE4.2)** instruction sets for acceleration on supported CPUs.
* It is extremely lightweight (only .h and .c files), perfect for high-performance micro-kernels.

### 4.2 Why limit Header count to 32?
```cpp
struct phr_header headers[32];
```
* **Speed vs. Safety Trade-off**:
    * Using a dynamic `std::vector` involves memory allocation.
    * Using a stack-based array is blazing fast.
    * 32 headers are sufficient for 99.9% of normal API requests. This is a **Fast Path** optimization for common scenarios.

### 4.3 Regarding `minor_version`
* Crucial for handling `Keep-Alive`:
    * HTTP/1.0: Defaults to closed (needs `Connection: keep-alive`).
    * HTTP/1.1: Defaults to open (needs `Connection: close` to shut down).