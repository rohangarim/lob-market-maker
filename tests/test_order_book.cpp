#include <gtest/gtest.h>

#include "orderbook/order_book.hpp"

using namespace lob;

namespace {
MarketEvent quote(SequenceNumber seq, Side side, double price, Quantity qty) {
    return MarketEvent::make_quote(Timestamp(seq), seq, Symbol("AAPL"), side,
                                    Price::from_double(price), qty);
}
MarketEvent del(SequenceNumber seq, Side side, double price) {
    return MarketEvent::make_delete(Timestamp(seq), seq, Symbol("AAPL"), side,
                                     Price::from_double(price));
}
}  // namespace

TEST(OrderBook, EmptyBookHasNoBestPrices) {
    OrderBook book(Symbol("AAPL"));
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    EXPECT_FALSE(book.mid_price().has_value());
    EXPECT_FALSE(book.spread().has_value());
}

TEST(OrderBook, InsertSingleLevelEachSide) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(quote(2, Side::Sell, 100.10, 30));

    ASSERT_TRUE(book.best_bid().has_value());
    ASSERT_TRUE(book.best_ask().has_value());
    EXPECT_DOUBLE_EQ(book.best_bid()->to_double(), 100.00);
    EXPECT_DOUBLE_EQ(book.best_ask()->to_double(), 100.10);
    EXPECT_NEAR(*book.mid_price(), 100.05, 1e-9);
    EXPECT_NEAR(*book.spread(), 0.10, 1e-9);
}

TEST(OrderBook, BidsSortedDescendingAsksAscending) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 99.90, 10));
    book.apply(quote(2, Side::Buy, 100.00, 10));
    book.apply(quote(3, Side::Buy, 99.95, 10));
    book.apply(quote(4, Side::Sell, 100.20, 10));
    book.apply(quote(5, Side::Sell, 100.10, 10));
    book.apply(quote(6, Side::Sell, 100.15, 10));

    auto bids = book.bid_depth(10);
    ASSERT_EQ(bids.size(), 3u);
    EXPECT_DOUBLE_EQ(bids[0].price.to_double(), 100.00);
    EXPECT_DOUBLE_EQ(bids[1].price.to_double(), 99.95);
    EXPECT_DOUBLE_EQ(bids[2].price.to_double(), 99.90);

    auto asks = book.ask_depth(10);
    ASSERT_EQ(asks.size(), 3u);
    EXPECT_DOUBLE_EQ(asks[0].price.to_double(), 100.10);
    EXPECT_DOUBLE_EQ(asks[1].price.to_double(), 100.15);
    EXPECT_DOUBLE_EQ(asks[2].price.to_double(), 100.20);
}

TEST(OrderBook, QuoteWithZeroQuantityRemovesLevel) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    EXPECT_EQ(book.bid_level_count(), 1u);
    book.apply(quote(2, Side::Buy, 100.00, 0));
    EXPECT_EQ(book.bid_level_count(), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(OrderBook, UpdateExistingLevelQuantity) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(quote(2, Side::Buy, 100.00, 75));
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(*book.best_bid_quantity(), 75);
}

TEST(OrderBook, ExplicitDeleteRemovesLevel) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(quote(2, Side::Buy, 99.95, 20));
    book.apply(del(3, Side::Buy, 100.00));
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_DOUBLE_EQ(book.best_bid()->to_double(), 99.95);
}

TEST(OrderBook, DeleteNonexistentLevelIsNoop) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(del(2, Side::Buy, 99.00));  // never existed
    EXPECT_EQ(book.bid_level_count(), 1u);
}

TEST(OrderBook, CrossedBookDetected) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.10, 10));
    book.apply(quote(2, Side::Sell, 100.00, 10));
    EXPECT_TRUE(book.crossed());
}

TEST(OrderBook, TradeDoesNotMutateLevels) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(quote(2, Side::Sell, 100.10, 50));
    book.apply(MarketEvent::make_trade(Timestamp(3), 3, Symbol("AAPL"), Side::Buy,
                                        Price::from_double(100.10), 10));
    EXPECT_EQ(book.bid_level_count(), 1u);
    EXPECT_EQ(book.ask_level_count(), 1u);
    ASSERT_TRUE(book.last_trade_price().has_value());
    EXPECT_DOUBLE_EQ(book.last_trade_price()->to_double(), 100.10);
}

TEST(OrderBook, SnapshotClearsBothSides) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 50));
    book.apply(quote(2, Side::Sell, 100.10, 50));
    book.apply(MarketEvent{Timestamp(3), 3, Symbol("AAPL"), EventType::Snapshot, {}, 0, Side::Buy});
    EXPECT_EQ(book.bid_level_count(), 0u);
    EXPECT_EQ(book.ask_level_count(), 0u);
}

TEST(OrderBook, DepthLimitedToRequestedLevels) {
    OrderBook book(Symbol("AAPL"));
    for (int i = 0; i < 10; ++i) {
        book.apply(quote(static_cast<SequenceNumber>(i + 1), Side::Buy, 100.00 - i * 0.01, 10));
    }
    auto bids = book.bid_depth(3);
    EXPECT_EQ(bids.size(), 3u);
    EXPECT_DOUBLE_EQ(bids[0].price.to_double(), 100.00);
}
