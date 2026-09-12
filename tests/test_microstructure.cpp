#include <gtest/gtest.h>

#include "metrics/microstructure.hpp"
#include "orderbook/order_book.hpp"

using namespace lob;

namespace {
MarketEvent quote(SequenceNumber seq, Side side, double price, Quantity qty, Timestamp ts) {
    return MarketEvent::make_quote(ts, seq, Symbol("AAPL"), side, Price::from_double(price), qty);
}
}  // namespace

TEST(Microstructure, SpreadBpsMatchesFormula) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 10, Timestamp(1)));
    book.apply(quote(2, Side::Sell, 100.10, 10, Timestamp(2)));

    auto bps = MicrostructureCalculator::spread_bps(book);
    ASSERT_TRUE(bps.has_value());
    // spread = 0.10, mid = 100.05 -> bps = 0.10/100.05 * 10000
    EXPECT_NEAR(*bps, 0.10 / 100.05 * 10000.0, 1e-6);
}

TEST(Microstructure, MicropriceLeansTowardSmallerSizeSide) {
    OrderBook book(Symbol("AAPL"));
    // Much larger size resting on the bid than the ask -> microprice should
    // lean toward the ask (the side more likely to move first).
    book.apply(quote(1, Side::Buy, 100.00, 1000, Timestamp(1)));
    book.apply(quote(2, Side::Sell, 100.10, 10, Timestamp(2)));

    auto mp = MicrostructureCalculator::microprice(book);
    ASSERT_TRUE(mp.has_value());
    double mid = *book.mid_price();
    EXPECT_GT(*mp, mid);  // leans toward the ask side
}

TEST(Microstructure, TopOfBookImbalanceSignAndRange) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 300, Timestamp(1)));
    book.apply(quote(2, Side::Sell, 100.10, 100, Timestamp(2)));

    auto oi = MicrostructureCalculator::top_of_book_imbalance(book);
    ASSERT_TRUE(oi.has_value());
    EXPECT_NEAR(*oi, (300.0 - 100.0) / (300.0 + 100.0), 1e-9);
    EXPECT_GE(*oi, -1.0);
    EXPECT_LE(*oi, 1.0);
}

TEST(Microstructure, DepthImbalanceAggregatesMultipleLevels) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(1, Side::Buy, 100.00, 100, Timestamp(1)));
    book.apply(quote(2, Side::Buy, 99.99, 100, Timestamp(1)));
    book.apply(quote(3, Side::Sell, 100.10, 50, Timestamp(2)));
    book.apply(quote(4, Side::Sell, 100.11, 50, Timestamp(2)));

    auto di = MicrostructureCalculator::depth_imbalance(book, 2);
    ASSERT_TRUE(di.has_value());
    EXPECT_NEAR(*di, (200.0 - 100.0) / (200.0 + 100.0), 1e-9);
}

TEST(Microstructure, VolatilityRequiresAtLeastTwoReturns) {
    OrderBook book(Symbol("AAPL"));
    MicrostructureCalculator micro;
    book.apply(quote(1, Side::Buy, 100.00, 10, Timestamp(1)));
    book.apply(quote(2, Side::Sell, 100.10, 10, Timestamp(2)));
    micro.on_book_update(book, Timestamp(2));
    EXPECT_FALSE(micro.short_term_volatility().has_value());

    book.apply(quote(3, Side::Buy, 100.02, 10, Timestamp(3)));
    micro.on_book_update(book, Timestamp(3));
    book.apply(quote(4, Side::Buy, 99.98, 10, Timestamp(4)));
    micro.on_book_update(book, Timestamp(4));
    EXPECT_TRUE(micro.short_term_volatility().has_value());
    EXPECT_GE(*micro.short_term_volatility(), 0.0);
}

TEST(Microstructure, RollingTradeVolumeSumsWindow) {
    MicrostructureCalculator micro(50, 3);
    micro.on_trade(10, Timestamp(std::chrono::seconds(1)));
    micro.on_trade(20, Timestamp(std::chrono::seconds(2)));
    micro.on_trade(30, Timestamp(std::chrono::seconds(3)));
    EXPECT_EQ(micro.rolling_trade_volume(), 60);
    micro.on_trade(5, Timestamp(std::chrono::seconds(4)));  // window size 3, oldest drops
    EXPECT_EQ(micro.rolling_trade_volume(), 55);
}

TEST(Microstructure, EmptyBookReturnsNulloptForAllMetrics) {
    OrderBook book(Symbol("AAPL"));
    EXPECT_FALSE(MicrostructureCalculator::spread_bps(book).has_value());
    EXPECT_FALSE(MicrostructureCalculator::microprice(book).has_value());
    EXPECT_FALSE(MicrostructureCalculator::top_of_book_imbalance(book).has_value());
}
