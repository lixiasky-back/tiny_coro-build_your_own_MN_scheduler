cmake_minimum_required(VERSION 3.20)

project(EbrScheduler LANGUAGES CXX C)

# ==========================================
# 1. C++ Standard Configuration
# ==========================================
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
add_compile_options(-stdlib=libc++)
link_libraries(-stdlib=libc++)
# ==========================================
# 2. Sanitizer Configuration
# ==========================================
option(ENABLE_ASAN "Enable AddressSanitizer" ON)
option(ENABLE_TSAN "Enable ThreadSanitizer" OFF)

set(SANITIZER_FLAGS "")
if(ENABLE_ASAN)
    message(STATUS "AddressSanitizer enabled")
    set(SANITIZER_FLAGS "-fsanitize=address -fno-omit-frame-pointer")
elseif(ENABLE_TSAN)
    message(STATUS "ThreadSanitizer enabled")
    set(SANITIZER_FLAGS "-fsanitize=thread")
endif()

# ==========================================
# 3. Platform & Compiler Optimizations
# ==========================================
add_compile_options(-Wall -Wextra ${SANITIZER_FLAGS})
add_link_options(${SANITIZER_FLAGS})

if(APPLE)
    add_compile_options(-O3)
    if(CMAKE_SYSTEM_PROCESSOR STREQUAL "arm64")
        add_compile_options(-march=armv8-a)
    endif()
elseif(UNIX AND NOT APPLE)
    add_compile_options(-O3 -march=native)
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
        add_compile_options(-Wno-unknown-pragmas)
    endif()
endif()

# ==========================================
# 4. Dependency & Includes
# ==========================================
find_package(Threads REQUIRED)

include_directories(include)

set(PICO_PARSER_SRC "${CMAKE_CURRENT_SOURCE_DIR}/include/picohttpparser/picohttpparser.c")

# ==========================================
# 5. Dynamically generate executable files (test mode)
# ==========================================

file(GLOB ALL_SOURCES "src/*.cpp")

foreach(source_file ${ALL_SOURCES})

    get_filename_component(exe_name ${source_file} NAME_WE)

    message(STATUS "Found Source: ${source_file} -> Creating Target: ${exe_name}")

    add_executable(${exe_name} ${source_file} ${PICO_PARSER_SRC})

    target_link_libraries(${exe_name} PRIVATE Threads::Threads)

    # GCC on Linux may require linking against libatomic
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_SYSTEM_NAME STREQUAL "Linux")
        target_link_libraries(${exe_name} PRIVATE atomic)
    endif()

endforeach()

# ==========================================
# 6. Build Summary
# ==========================================
message(STATUS "--------------------------------------------")
message(STATUS "Mode: Batch Test Build")
message(STATUS "Compiler: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")
message(STATUS "Common C Dependency: ${PICO_PARSER_SRC}")
message(STATUS "--------------------------------------------")