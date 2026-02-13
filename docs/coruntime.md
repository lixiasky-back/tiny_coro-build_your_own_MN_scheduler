# C++20 Coroutines: From Introduction to Hand-coding a Scheduler

## 0. Core Philosophy: It's a "State Machine"

Don't be intimidated by the high-sounding name "Coroutine." The essence of C++20 coroutines is that the compiler does the dirty work for you, quietly rewriting a function you wrote into a **"Heap-allocated State Machine"**.

* **Normal Functions**: Call -> Push to Stack -> Execute -> Pop from Stack -> Destroy. It happens in one go and cannot be stopped in the middle.
* **Coroutine Functions**: Call -> **Allocate memory on the heap (Coroutine Frame)** -> Store local variables in it -> The function can now "suspend" -> It can be retrieved via a **Handle** anytime later to resume from where it left off.



To master C++20 coroutines, you only need to understand the relationship between three roles:
1.  **Promise** (The Director): Controls the coroutine's lifecycle, return values, and exceptions.
2.  **Awaitable** (The Pause Switch): Determines how, how long, and how the coroutine wakes up during a `co_await`.
3.  **Handle** (The Remote Control): Used externally (e.g., by a scheduler) to control the resumption and destruction of the coroutine.

---

## 1. Stage 1: Defining the "Shell" (Task & Promise)

In C++20, any function containing `co_await`, `co_return`, or `co_yield` is a coroutine.
However, the return type of a coroutine function cannot be arbitrary (it cannot be `void` or `int`); it must be a class that satisfies specific rules. In your project, this class is `Task`.

### 1.1 `struct Task`: The Return Value for Users
`Task` is the contract between the coroutine and the caller.

```cpp
// Simplified version of tiny_coro code
struct Task {
    // You must define a nested type called promise_type! 
    // The compiler only recognizes this name!
    struct Promise; 
    using promise_type = Promise;

    // The only member variable: The coroutine's remote control
    std::coroutine_handle<Promise> handle;
    
    // ... Constructor/Destructor/RAII management ...
};
```

### 1.2 `struct Promise`: The Man Behind the Curtain
This is the most difficult part to grasp. When the compiler generates coroutine code, it first creates this object. It acts like the "brain" of the coroutine.

**How does the `Promise` in tiny_coro's `task.h` work?**

```cpp
struct Promise {
    // 1. Should the coroutine suspend immediately upon creation or run directly?
    // return std::suspend_always{} -> Suspend immediately after creation (Lazy Start).
    // tiny_coro's choice: suspend_always. Because you want to throw the Task into 
    // a thread pool instead of running it directly on the current thread.
    std::suspend_always initial_suspend() noexcept { return {}; }

    // 2. What happens after the coroutine executes the last line of code (or co_return)?
    // return std::suspend_always{} -> Stay suspended, don't destroy the coroutine frame yet.
    // Why? Because if destroyed immediately, the Task object might still be waiting 
    // to get a return value, leading to a dangling pointer.
    // tiny_coro defines FinalAwaiter here to handle ref-counting and resource cleanup.
    FinalAwaiter final_suspend() noexcept { return {}; }

    // 3. How is the Task object created?
    // The compiler calls this function to provide the return value to the caller.
    Task get_return_object() {
        return Task{std::coroutine_handle<Promise>::from_promise(*this)};
    }

    void return_void() {} 
    void unhandled_exception() { std::terminate(); }
};
```



---

## 2. Stage 2: The Art of Pausing (Awaitable & co_await)

`co_await` is the soul of coroutines. When you write `co_await socket.read()`, the compiler translates this line into a series of complex calls. The object returned (e.g., `AsyncReadAwaiter`) must implement three "magic methods."

### 2.1 `await_ready`: Can we skip the queue?
* **Ready (true)**: No one is in line. You **don't need to suspend**; proceed immediately (The Fast Path).
* **Not Ready (false)**: Someone is waiting; you must fill out a form and wait in line (Enter `await_suspend`).

### 2.2 `await_suspend`: Where to go after pausing?
This is the most critical step. When `await_ready` returns `false`, the coroutine is officially suspended.

```cpp
// h is the handle (remote control) for the current coroutine
void await_suspend(std::coroutine_handle<> h) {
    // 1. Register: Hand the handle to the Reactor (epoll)
    reactor_->register_read(fd_, h.address());
    
    // 2. Exit: Function ends, the current Worker thread is now free!
    // It can execute the next Task in the scheduling queue instead of waiting idly.
}
```



### 2.3 `await_resume`: Getting results upon waking up
When the Reactor finds data is ready, it calls `h.resume()`. The coroutine wakes from its slumber and enters this function. The return value of this function becomes the result of the `co_await` expression.

---

## 3. Stage 3: Scheduler Magic (M:N Model)

In the `tiny_coro` framework, coroutines don't stay on one thread; they "teleport."

**Scenario Recap:**
1.  **Thread A**'s coroutine reaches `co_await socket.read()`.
2.  `await_suspend` is called, and the handle is registered to the **Reactor**.
3.  **Thread A** is now free and "steals" another task from the queue.
4.  **Reactor Thread** detects the Socket is readable and retrieves handle `h`.
5.  Reactor calls `scheduler->spawn(h)`, throwing the handle into the **Global Queue**.
6.  **Thread B** (potentially a different thread!) finds `h` in the queue and executes `h.resume()`.
7.  The coroutine is resurrected on **Thread B**.



---

## 4. Advanced Techniques in Your Code

### 4.1 Why is `Task::detach()` so important?
`Task` is an RAII object. When you `spawn(my_coro())`, the temporary `Task` would normally be destructed (destroying the coroutine) as soon as `spawn` ends. `detach()` tells the wrapper: "I'll take the raw pointer; let the scheduler manage the lifecycle manually."

### 4.2 Why does `final_suspend` return `std::noop_coroutine`?
This utilizes **Symmetric Transfer**. It tells the compiler: "I am suspending; transfer control directly to coroutine B." This enables **Tail Call Optimization**, allowing you to nest `co_await` infinitely without blowing the stack.



---

## 5. Summary: Three Rules for Coroutines

1.  **`Task` is the Shell, `Promise` is the Core**: `Task` is for the user, `Promise` is for the compiler.
2.  **`await_ready` for Fast Path, `await_suspend` for Registration**: This is the key to high-performance async I/O.
3.  **`Handle` is the Remote Control; lose it, and you lose contact**: Everything is on the heap; the `Handle` is your only index. A scheduler is essentially a manager of these `Handle` collections.