#pragma once

#include <atomic>
#include <cstddef>
#include <new>
#include <optional>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// Bounded single-producer/single-consumer lock-free ring buffer.
//
// Why lock-free here and not a mutex-guarded std::queue:
//   The market-data ingestion thread (producer) must never block on the
//   consumer (order-book/strategy thread). A mutex introduces priority
//   inversion risk and unbounded producer-side latency if the consumer is
//   scheduled out mid-critical-section. A wait-free (for the producer) /
//   lock-free (for the consumer) ring buffer bounds the worst case to a
//   handful of atomic loads and a memcpy-sized element copy, with no
//   syscalls and no possibility of the producer sleeping on a futex.
//
// Ownership model:
//   Exactly one thread may call push() (the producer) and exactly one
//   thread may call pop() (the consumer). This is enforced by convention,
//   not by the type system -- callers must guarantee single-producer/
//   single-consumer usage. Mixing multiple producers or consumers without
//   external synchronization is undefined behavior (the head/tail indices
//   are not fetch_add-safe against concurrent same-side callers).
//
// Memory ordering:
//   - head_ (write index) is written by the producer with release ordering
//     after the slot's payload has been stored with a plain (non-atomic)
//     write. This makes the payload write visible-before the index bump is
//     observed by the consumer.
//   - head_ is read by the consumer with acquire ordering before it reads
//     the slot payload, establishing a happens-before edge with the
//     producer's release store.
//   - tail_ (read index) is written by the consumer with release ordering
//     after it has finished reading the slot, and read by the producer
//     with acquire ordering to determine free capacity. This prevents the
//     producer from overwriting a slot the consumer has not yet consumed.
//   - Relaxed loads are used for a thread's *own* cached copy of the other
//     side's index (e.g. producer's cached_tail_) purely as an
//     optimization to avoid a cross-core fetch on every push; correctness
//     never depends on the relaxed load being fresh, only the acquire load
//     taken on the slow path when the cache appears stale.
//
// Cache-line behavior / false sharing:
//   head_ and tail_ are each pinned to their own cache line (via
//   hardware_destructive_interference_size padding). Without this padding,
//   the producer's frequent writes to head_ and the consumer's frequent
//   writes to tail_ would ping-pong the same cache line between cores on
//   every operation (false sharing), serializing what should be
//   independent, parallel progress. The producer's and consumer's cached
//   copies of the *other* side's index are also padded away from their own
//   index to avoid the same problem.
//
// Overflow behavior:
//   push() returns false when the buffer is full; it never blocks and
//   never overwrites unread data. Callers decide the drop policy (e.g.
//   count and log a dropped-event metric). This is a deliberate choice:
//   silently overwriting market-data events would corrupt sequence-number
//   continuity checks downstream.
// ---------------------------------------------------------------------------
template <typename T, size_t Capacity>
class SpscRingBuffer {
    static_assert(Capacity > 1 && (Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");

public:
    SpscRingBuffer() : buffer_(Capacity) {}

    SpscRingBuffer(const SpscRingBuffer&) = delete;
    SpscRingBuffer& operator=(const SpscRingBuffer&) = delete;

    // Producer side only.
    bool push(const T& value) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;

        if (next == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next == cached_tail_) {
                return false;  // full
            }
        }

        buffer_[head] = value;
        head_.store(next, std::memory_order_release);
        return true;
    }

    bool push(T&& value) noexcept {
        const size_t head = head_.load(std::memory_order_relaxed);
        const size_t next = (head + 1) & kMask;

        if (next == cached_tail_) {
            cached_tail_ = tail_.load(std::memory_order_acquire);
            if (next == cached_tail_) {
                return false;  // full
            }
        }

        buffer_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    // Consumer side only.
    std::optional<T> pop() noexcept {
        const size_t tail = tail_.load(std::memory_order_relaxed);

        if (tail == cached_head_) {
            cached_head_ = head_.load(std::memory_order_acquire);
            if (tail == cached_head_) {
                return std::nullopt;  // empty
            }
        }

        T value = std::move(buffer_[tail]);
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return value;
    }

    [[nodiscard]] bool empty() const noexcept {
        return head_.load(std::memory_order_acquire) ==
               tail_.load(std::memory_order_acquire);
    }

    [[nodiscard]] static constexpr size_t capacity() noexcept { return Capacity - 1; }

private:
    static constexpr size_t kMask = Capacity - 1;

#ifdef __cpp_lib_hardware_interference_size
    static constexpr size_t kCacheLine = std::hardware_destructive_interference_size;
#else
    static constexpr size_t kCacheLine = 64;
#endif

    std::vector<T> buffer_;

    alignas(kCacheLine) std::atomic<size_t> head_{0};
    // Producer's private cached view of the consumer's tail index. Lives on
    // the producer's cache line, not the shared tail_ line, so the
    // consumer's writes to tail_ don't invalidate producer-local reads of
    // this field between the (infrequent) refreshes above.
    alignas(kCacheLine) size_t cached_tail_ = 0;

    alignas(kCacheLine) std::atomic<size_t> tail_{0};
    alignas(kCacheLine) size_t cached_head_ = 0;
};

}  // namespace lob
