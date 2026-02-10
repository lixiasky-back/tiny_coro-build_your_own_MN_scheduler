cmake_minimum_required(VERSION 3.20)
project(EbrScheduler LANGUAGES CXX)

# ==========================================
# 1. C++ standard
# ==========================================
# Require C++20 (GCC 10+/Clang 10+ for coroutines)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Std C++ only: -std=c++20 (no gnu++20)

# ==========================================
# 2. Platform & compiler optimizations
# ==========================================

# Warnings: GCC & Clang compatible
add_compile_options(-Wall -Wextra)

if(APPLE)
    # --- mmacOS-only config ---
    message(STATUS "Configuring for macOS...")
    add_compile_options(-O3)

    # Apple Silicon Opt
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_compile_options(-march=armv8-a)
    endif()

elseif(UNIX AND NOT APPLE)
    # --- Linux (GCC/Clang) config ---
    message(STATUS "Configuring for Linux...")

    # -O3: Highest level of optimization
    # -march=native: Extremely important! Tells GCC to generate optimal code for the CPU of the machine running this program
    # (e.g., automatically enables instruction sets like AVX2, AVX512, BMI2, which are critical for lock-free queue performance)
    add_compile_options(-O3 -march=native)

    # GCC only: suppress false coroutine warnings
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()
endif()

# ==========================================
# 3. dependency_lookup
# ==========================================

# Lookup pthread
find_package(Threads REQUIRED)

# Include header dirs
include_directories(include)

# ==========================================
# 4. Define target (executable/library)
# ==========================================

# Add all files to target (IDE-friendly)
add_executable(scheduler src/main.cpp
        include/parker.h
        include/ebr.h

)

# ==========================================
# 5. Link libs
# ==========================================

target_link_libraries(scheduler PRIVATE Threads::Threads)

# [Critical] GCC on Linux usually requires explicit linking of libatomic
# If 16-byte CAS is used in the code (e.g., std::atomic<std::shared_ptr>), linking may fail without this library
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_libraries(scheduler PRIVATE atomic)
endif()

# 打印构建信息
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Build Type: ${CMAKE_BUILD_TYPE}")