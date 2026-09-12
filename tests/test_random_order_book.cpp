#include <gtest/gtest.h>

#include <random>
#include <set>

#include "orderbook/order_book.hpp"

using namespace lob;

namespace {

void assert_invariants(const OrderBook& book) {
    auto bids = book.bid_depth(1000);
    auto asks = book.ask_depth(1000);

    // Bids strictly descending, asks strictly ascending, no duplicate prices
    // on either side (each price level appears at most once).
    std::set<int64_t> seen_bid_prices;
    for (size_t i = 0; i < bids.size(); ++i) {
        ASSERT_TRUE(seen_bid_prices.insert(bids[i].price.ticks()).second)
            << "duplicate bid price level";
        ASSERT_GT(bids[i].quantity, 0);
        if (i > 0) ASSERT_LT(bids[i].price.ticks(), bids[i - 1].price.ticks());
    }
    std::set<int64_t> seen_ask_prices;
    for (size_t i = 0; i < asks.size(); ++i) {
        ASSERT_TRUE(seen_ask_prices.insert(asks[i].price.ticks()).second)
            << "duplicate ask price level";
        ASSERT_GT(asks[i].quantity, 0);
        if (i > 0) ASSERT_GT(asks[i].price.ticks(), asks[i - 1].price.ticks());
    }
}

}  // namespace

TEST(OrderBookRandomized, MaintainsSortedNoDuplicatePositiveQtyInvariants) {
    std::mt19937_64 rng(20240613);
    std::uniform_real_distribution<double> price_dist(90.0, 110.0);
    std::uniform_int_distribution<Quantity> qty_dist(0, 200);  // qty 0 exercises deletion path
    std::uniform_int_distribution<int> side_dist(0, 1);
    std::uniform_int_distribution<int> action_dist(0, 9);

    OrderBook book(Symbol("AAPL"));
    SequenceNumber seq = 0;

    for (int iter = 0; iter < 20'000; ++iter) {
        Side side = side_dist(rng) == 0 ? Side::Buy : Side::Sell;
        double raw_price = std::round(price_dist(rng) * 100.0) / 100.0;  // penny-aligned
        Price price = Price::from_double(raw_price);
        ++seq;

        if (action_dist(rng) < 8) {
            book.apply(MarketEvent::make_quote(Timestamp(iter), seq, Symbol("AAPL"), side, price,
                                                qty_dist(rng)));
        } else {
            book.apply(MarketEvent::make_delete(Timestamp(iter), seq, Symbol("AAPL"), side, price));
        }

        // Not asserting an uncrossed book here: random quotes can
        // legitimately cross since this fuzz test doesn't model a matching
        // engine upstream. We only assert the book's own bookkeeping stays
        // internally consistent (sorted, unique, positive quantities).
        assert_invariants(book);
    }
}

TEST(OrderBookRandomized, SnapshotResetIsAlwaysSafeMidStream) {
    std::mt19937_64 rng(99);
    std::uniform_real_distribution<double> price_dist(90.0, 110.0);
    std::uniform_int_distribution<Quantity> qty_dist(1, 200);

    OrderBook book(Symbol("AAPL"));
    for (int iter = 0; iter < 5000; ++iter) {
        SequenceNumber seq = static_cast<SequenceNumber>(iter + 1);
        if (iter % 500 == 499) {
            book.apply(MarketEvent{Timestamp(iter), seq, Symbol("AAPL"), EventType::Snapshot,
                                    {}, 0, Side::Buy});
            EXPECT_EQ(book.bid_level_count(), 0u);
            EXPECT_EQ(book.ask_level_count(), 0u);
            continue;
        }
        Side side = (iter % 2 == 0) ? Side::Buy : Side::Sell;
        book.apply(MarketEvent::make_quote(Timestamp(iter), seq, Symbol("AAPL"), side,
                                            Price::from_double(price_dist(rng)), qty_dist(rng)));
        assert_invariants(book);
    }
}
