#pragma once

#include <cstddef>
#include <memory>
#include <stdexcept>
#include <vector>

namespace lob {

// ---------------------------------------------------------------------------
// Fixed-capacity intrusive free-list object pool.
//
// Purpose: avoid calling new/delete (and the malloc bookkeeping/locking that
// comes with it) for objects created and destroyed at market-data-event
// rates. Backing storage is a single contiguous allocation made once at
// construction; acquire()/release() are O(1) pointer manipulation with no
// syscalls and no heap metadata touched on the hot path.
//
// Not thread-safe. Intended for single-owner use (one thread acquires and
// releases), matching the single-writer order-book/strategy thread in this
// project. A pool shared across threads would need external synchronization
// or a lock-free free-list, which this project does not need.
// ---------------------------------------------------------------------------
template <typename T>
class ObjectPool {
    union Slot {
        Slot() {}
        ~Slot() {}
        T value;
        Slot* next_free;
    };

public:
    explicit ObjectPool(size_t capacity)
        : storage_(std::make_unique<Slot[]>(capacity)), capacity_(capacity) {
        for (size_t i = 0; i + 1 < capacity_; ++i) {
            storage_[i].next_free = &storage_[i + 1];
        }
        if (capacity_ > 0) {
            storage_[capacity_ - 1].next_free = nullptr;
            free_head_ = &storage_[0];
        }
    }

    ObjectPool(const ObjectPool&) = delete;
    ObjectPool& operator=(const ObjectPool&) = delete;

    template <typename... Args>
    T* acquire(Args&&... args) {
        if (free_head_ == nullptr) {
            ++exhausted_count_;
            return nullptr;  // pool exhausted; caller decides fallback policy
        }
        Slot* slot = free_head_;
        free_head_ = slot->next_free;
        ++live_count_;
        return ::new (&slot->value) T(std::forward<Args>(args)...);
    }

    void release(T* obj) noexcept {
        if (obj == nullptr) return;
        obj->~T();
        Slot* slot = reinterpret_cast<Slot*>(obj);
        slot->next_free = free_head_;
        free_head_ = slot;
        --live_count_;
    }

    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] size_t live_count() const noexcept { return live_count_; }
    [[nodiscard]] size_t exhausted_count() const noexcept { return exhausted_count_; }

private:
    std::unique_ptr<Slot[]> storage_;
    size_t capacity_;
    Slot* free_head_ = nullptr;
    size_t live_count_ = 0;
    size_t exhausted_count_ = 0;
};

}  // namespace lob
