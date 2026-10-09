#include <gtest/gtest.h>

#include "orderbook/order_book.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;

TEST(SyntheticBook, DeterministicForSameSeed) {
    SyntheticBookGenerator a(7, 190.0, 0.01, Symbol("AAPL"));
    SyntheticBookGenerator b(7, 190.0, 0.01, Symbol("AAPL"));
    SyntheticBookGenerator c(8, 190.0, 0.01, Symbol("AAPL"));
    bool differs = false;
    for (int i = 0; i < 2000; ++i) {
        auto x = a.next(), y = b.next(), z = c.next();
        ASSERT_EQ(x.sequence, y.sequence);
        ASSERT_EQ(x.type, y.type);
        ASSERT_EQ(x.price.ticks(), y.price.ticks());
        ASSERT_EQ(x.quantity, y.quantity);
        if (x.price.ticks() != z.price.ticks() || x.type != z.type) differs = true;
    }
    EXPECT_TRUE(differs);
}

TEST(SyntheticBook, SequencesConsecutiveAndTimestampsNonDecreasing) {
    SyntheticBookGenerator g(1, 190.0, 0.01, Symbol("AAPL"));
    auto prev = g.next();
    EXPECT_EQ(prev.sequence, 1u);
    for (int i = 0; i < 50000; ++i) {
        auto e = g.next();
        ASSERT_EQ(e.sequence, prev.sequence + 1);
        ASSERT_GE(e.timestamp, prev.timestamp);
        prev = e;
    }
}

// Guards against the earlier flaw where stale levels accumulated forever and
// the book sat permanently crossed with thousands of levels.
TEST(SyntheticBook, BookStaysBoundedAndUncrossed) {
    SyntheticBookGenerator g(1234, 190.0, 0.01, Symbol("AAPL"));
    OrderBook book(Symbol("AAPL"));
    const int kEvents = 400000;
    size_t crossed = 0, max_levels = 0, two_sided = 0;
    for (int i = 0; i < kEvents; ++i) {
        book.apply(g.next());
        if (book.best_bid() && book.best_ask()) {
            ++two_sided;
            if (book.crossed()) ++crossed;
        }
        max_levels = std::max(max_levels, book.bid_level_count() + book.ask_level_count());
    }
    EXPECT_GT(two_sided, static_cast<size_t>(kEvents) / 2);
    EXPECT_LE(crossed, static_cast<size_t>(kEvents) / 1000);
    EXPECT_LE(max_levels, 30u);
}

TEST(SyntheticBook, ProducesAllEventKinds) {
    SyntheticBookGenerator g(3, 190.0, 0.01, Symbol("AAPL"));
    size_t counts[5] = {};
    for (int i = 0; i < 20000; ++i) ++counts[static_cast<int>(g.next().type)];
    EXPECT_GT(counts[static_cast<int>(EventType::Quote)], 0u);
    EXPECT_GT(counts[static_cast<int>(EventType::Trade)], 0u);
    EXPECT_GT(counts[static_cast<int>(EventType::BookDelete)], 0u);
    EXPECT_GT(counts[static_cast<int>(EventType::Heartbeat)], 0u);
}
