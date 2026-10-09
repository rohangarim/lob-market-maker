#include <gtest/gtest.h>

#include <atomic>
#include <numeric>
#include <thread>

#include "concurrency/spsc_ring_buffer.hpp"

using namespace lob;

TEST(SpscRingBuffer, EmptyOnConstruction) {
    SpscRingBuffer<int, 8> rb;
    EXPECT_TRUE(rb.empty());
    EXPECT_FALSE(rb.pop().has_value());
}

TEST(SpscRingBuffer, PushPopPreservesOrder) {
    SpscRingBuffer<int, 8> rb;
    for (int i = 0; i < 5; ++i) EXPECT_TRUE(rb.push(i));
    for (int i = 0; i < 5; ++i) {
        auto v = rb.pop();
        ASSERT_TRUE(v.has_value());
        EXPECT_EQ(*v, i);
    }
    EXPECT_FALSE(rb.pop().has_value());
}

TEST(SpscRingBuffer, ReportsFullWithoutOverwriting) {
    SpscRingBuffer<int, 4> rb;  // usable capacity = 3
    EXPECT_EQ(rb.capacity(), 3u);
    EXPECT_TRUE(rb.push(1));
    EXPECT_TRUE(rb.push(2));
    EXPECT_TRUE(rb.push(3));
    EXPECT_FALSE(rb.push(4));  // full: must not overwrite unread data

    EXPECT_EQ(*rb.pop(), 1);
    EXPECT_EQ(*rb.pop(), 2);
    EXPECT_EQ(*rb.pop(), 3);
    EXPECT_FALSE(rb.pop().has_value());
}

TEST(SpscRingBuffer, WrapsAroundCorrectly) {
    SpscRingBuffer<int, 4> rb;
    for (int round = 0; round < 100; ++round) {
        EXPECT_TRUE(rb.push(round));
        EXPECT_TRUE(rb.push(round * 2));
        EXPECT_EQ(*rb.pop(), round);
        EXPECT_EQ(*rb.pop(), round * 2);
    }
}

TEST(SpscRingBuffer, ConcurrentProducerConsumerDeliversAllItemsInOrder) {
    constexpr int kCount = 200'000;
    SpscRingBuffer<int, 1024> rb;
    std::vector<int> consumed;
    consumed.reserve(kCount);

    std::thread producer([&] {
        int i = 0;
        while (i < kCount) {
            if (rb.push(i)) ++i;
        }
    });

    std::thread consumer([&] {
        while (static_cast<int>(consumed.size()) < kCount) {
            if (auto v = rb.pop()) consumed.push_back(*v);
        }
    });

    producer.join();
    consumer.join();

    ASSERT_EQ(consumed.size(), static_cast<size_t>(kCount));
    for (int i = 0; i < kCount; ++i) {
        ASSERT_EQ(consumed[static_cast<size_t>(i)], i) << "order violated at index " << i;
    }
}

TEST(SpscRingBuffer, OverflowBurstKeepsOldestAndRejectsTheRest) {
    SpscRingBuffer<int, 8> rb;  // usable capacity 7
    int accepted = 0, dropped = 0;
    for (int i = 0; i < 1000; ++i) (rb.push(i) ? accepted : dropped)++;
    EXPECT_EQ(accepted, 7);
    EXPECT_EQ(dropped, 993);
    for (int i = 0; i < 7; ++i) EXPECT_EQ(*rb.pop(), i) << "an unread slot was overwritten";
    EXPECT_FALSE(rb.pop().has_value());
    EXPECT_TRUE(rb.push(1000));  // space is reusable after draining
}

TEST(SpscRingBuffer, FastProducerSlowConsumerAccountsForEveryItem) {
    constexpr int kAttempts = 200'000;
    SpscRingBuffer<int, 64> rb;
    std::atomic<bool> done{false};
    int accepted = 0, dropped = 0;
    std::vector<int> received;

    std::thread producer([&] {
        for (int i = 0; i < kAttempts; ++i) (rb.push(i) ? accepted : dropped)++;
        done.store(true, std::memory_order_release);
    });
    std::thread consumer([&] {
        while (true) {
            if (auto v = rb.pop()) {
                received.push_back(*v);
                if (received.size() % 16 == 0) std::this_thread::yield();  // stay slower than the producer
            } else if (done.load(std::memory_order_acquire) && rb.empty()) {
                break;
            }
        }
    });
    producer.join();
    consumer.join();

    EXPECT_EQ(accepted + dropped, kAttempts);
    EXPECT_EQ(static_cast<int>(received.size()), accepted);
    for (size_t i = 1; i < received.size(); ++i) ASSERT_LT(received[i - 1], received[i]);
}
