// Licensed under MIT (Commercial Allowed). See DUAL_LICENSING_NOTICE for details.
#pragma once
#include <chrono>
#include <coroutine>

using TimePoint = std::chrono::steady_clock::time_point;

struct Timer {
    TimePoint expiry;
    std::coroutine_handle<> handle;

    // Key: To make priority_queue a min-heap,
    // we need to use it with std::greater, and std::greater internally calls operator>.
    // Logic: If A's time > B's time, then A is "greater", A will sink to the bottom, and B will float up.
    bool operator>(const Timer& other) const {
        return expiry > other.expiry;
    }
};