#pragma once

#include <chrono>
#include <cstdint>

#include "concurrency/spsc_ring_buffer.hpp"
#include "marketdata/events.hpp"

namespace lob {

// A MarketEvent plus the steady-clock instant it entered the ring buffer, so the
// consumer can measure how long events wait in the queue.
struct QueuedEvent {
    MarketEvent event;
    int64_t enqueue_steady_ns = 0;
};

inline int64_t steady_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

using EventRing = SpscRingBuffer<QueuedEvent, 1 << 16>;

// Producer-side helper: stamps and pushes; returns false if the ring was full.
inline bool enqueue(EventRing& ring, const MarketEvent& ev) {
    return ring.push(QueuedEvent{ev, steady_now_ns()});
}

}  // namespace lob
