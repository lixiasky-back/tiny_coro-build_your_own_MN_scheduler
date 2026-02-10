#pragma once
#include <atomic>

class Parker {
private:
    enum State { EMPTY = 0, PARKED = 1, NOTIFIED = 2 };
    std::atomic<int> state{EMPTY};

public:
    void park() {
        // // PARK only if EMPTY
        int expected = EMPTY;
        if (state.compare_exchange_strong(expected, PARKED, std::memory_order_acquire)) {
            while (state.load(std::memory_order_acquire) == PARKED) {
                state.wait(PARKED);
            }
        }
        // Reset state: awakened or NOTIFIED
        state.store(EMPTY, std::memory_order_release);
    }

    void unpark() {
        // Set to NOTIFIED (ignore current state)
        int old = state.exchange(NOTIFIED, std::memory_order_release);
        if (old == PARKED) {
            state.notify_one();
        }
    }
};