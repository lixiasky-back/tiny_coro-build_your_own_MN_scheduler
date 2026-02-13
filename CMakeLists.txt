cmake_minimum_required(VERSION 3.20)
project(EbrScheduler LANGUAGES CXX)

# ==========================================
# 1. C++ Standard Configuration
# ==========================================
# Require C++20 (Required for Coroutines: GCC 10+ / Clang 10+)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Standard C++ only: -std=c++20 (no gnu++20)

# ==========================================
# 2. Sanitizer Configuration (ASAN/TSAN)
# ==========================================
# Note: ASAN and TSAN usually cannot be used simultaneously.
option(ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(SANITIZER_FLAGS "")
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    # -fno-omit-frame-pointer helps produce better stack traces
    set(SANITIZER_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
elseif(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    set(SANITIZER_FLAGS "-fsanitize=thread")
endif()

# ==========================================
# 3. Platform & Compiler Optimizations
# ==========================================

# General warnings and sanitizer flags
add_compile_options(-Wall -Wextra ${SANITIZER_FLAGS})
# Sanitizers must be passed to the linker as well
add_link_options(${SANITIZER_FLAGS})

if(APPLE)
    # --- macOS-specific configuration ---
    message(STATUS "Configuring for macOS...")
    add_compile_options(-O3)

    # Apple Silicon Optimization
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_compile_options(-march=armv8-a)
    endif()

elseif(UNIX AND NOT APPLE)
    # --- Linux (GCC/Clang) configuration ---
    message(STATUS "Configuring for Linux...")

    # -O3: Highest level of optimization
    # -march=native: Generates code for the local CPU (enables AVX2, AVX512, BMI2, etc.)
    # This is critical for high-performance lock-free data structures.
    add_compile_options(-O3 -march=native)

    # Suppress false coroutine warnings on GCC
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()
endif()

# ==========================================
# 4. Dependency Lookup
# ==========================================

# Find pthread
find_package(Threads REQUIRED)

# Include header directories
include_directories(include)

# ==========================================
# 5. Define Target (Changed to 'example')
# ==========================================

# Add all source and header files to target for IDE visibility
add_executable(example src/simple_http_web.cpp
        include/parker.h
        include/ebr.h
        include/task.h
        include/queue.h
        include/worker.h
        include/poller.h
        include/scheduler.h
        include/timer.h
        include/socket.h
        include/picohttpparser/picohttpparser.h
        include/picohttpparser/picohttpparser.c
        include/http/http_parser.h
        include/http/http_server.h
)

# ==========================================
# 6. Linking
# ==========================================

target_link_libraries(example PRIVATE Threads::Threads)

# GCC on Linux requires libatomic for certain atomic operations (e.g., 16-byte CAS)
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(example PRIVATE atomic)
endif()

# Print Build Summary
message(STATUS "--------------------------------------------")
message(STATUS "Target: example")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")
message(STATUS "Sanitizers: ${SANITIZER_FLAGS}")
message(STATUS "--------------------------------------------")