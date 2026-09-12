#include <gtest/gtest.h>

#include "execution/paper_execution_engine.hpp"
#include "orderbook/order_book.hpp"

using namespace lob;

namespace {
MarketEvent trade(SequenceNumber seq, Side side, double price, Quantity qty, Timestamp ts) {
    return MarketEvent::make_trade(ts, seq, Symbol("AAPL"), side, Price::from_double(price), qty);
}
}  // namespace

TEST(PaperExecutionEngine, SubmitLimitCreatesOpenOrder) {
    PaperExecutionEngine engine;
    auto report = engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.0), 10,
                                       Timestamp(1));
    EXPECT_EQ(report.status, OrderStatus::New);
    EXPECT_EQ(engine.open_order_count(), 1u);
    EXPECT_EQ(engine.total_submitted(), 1u);
}

TEST(PaperExecutionEngine, CancelRemovesOpenOrder) {
    PaperExecutionEngine engine;
    engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.0), 10, Timestamp(1));
    auto report = engine.cancel(1, Timestamp(2));
    ASSERT_TRUE(report.has_value());
    EXPECT_EQ(report->status, OrderStatus::Canceled);
    EXPECT_EQ(engine.open_order_count(), 0u);
    EXPECT_EQ(engine.total_canceled(), 1u);
}

TEST(PaperExecutionEngine, CancelUnknownOrderReturnsNullopt) {
    PaperExecutionEngine engine;
    EXPECT_FALSE(engine.cancel(999, Timestamp(1)).has_value());
}

TEST(PaperExecutionEngine, RestingBuyLimitFillsWhenTradePrintsThroughIt) {
    FillAssumptions fa;
    fa.partial_fill_probability = 0.0;  // force full fills for a deterministic test
    PaperExecutionEngine engine(fa);
    engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.00), 20, Timestamp(1));

    // A sell-side trade at or below our buy limit price crosses it.
    auto reports = engine.on_market_event(trade(2, Side::Sell, 99.99, 20, Timestamp(2)));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, OrderStatus::Filled);
    EXPECT_EQ(reports[0].fill_quantity, 20);
    EXPECT_DOUBLE_EQ(reports[0].fill_price.to_double(), 100.00);  // fills at resting limit price
    EXPECT_EQ(engine.open_order_count(), 0u);
}

TEST(PaperExecutionEngine, RestingLimitDoesNotFillWhenTradeDoesNotCross) {
    PaperExecutionEngine engine;
    engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.00), 20, Timestamp(1));

    auto reports = engine.on_market_event(trade(2, Side::Sell, 100.50, 20, Timestamp(2)));
    EXPECT_TRUE(reports.empty());
    EXPECT_EQ(engine.open_order_count(), 1u);
}

TEST(PaperExecutionEngine, PartialFillLeavesOrderOpenWithReducedRemaining) {
    FillAssumptions fa;
    fa.partial_fill_probability = 1.0;  // force partial
    fa.min_partial_fill_ratio = 0.5;
    fa.max_partial_fill_ratio = 0.5;  // deterministic 50% partial
    PaperExecutionEngine engine(fa);
    engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.00), 20, Timestamp(1));

    auto reports = engine.on_market_event(trade(2, Side::Sell, 99.99, 100, Timestamp(2)));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(reports[0].fill_quantity, 10);
    EXPECT_EQ(reports[0].remaining_quantity, 10);
    EXPECT_EQ(engine.open_order_count(), 1u);
    EXPECT_EQ(engine.total_partial_fills(), 1u);
}

TEST(PaperExecutionEngine, FillQuantityNeverExceedsTradeQuantity) {
    FillAssumptions fa;
    fa.partial_fill_probability = 0.0;
    PaperExecutionEngine engine(fa);
    engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.00), 50, Timestamp(1));

    // Trade only prints 5 shares -- the resting order cannot fill for more
    // than actually traded even though it wants 50.
    auto reports = engine.on_market_event(trade(2, Side::Sell, 99.99, 5, Timestamp(2)));
    ASSERT_EQ(reports.size(), 1u);
    EXPECT_EQ(reports[0].fill_quantity, 5);
    EXPECT_EQ(engine.open_order_count(), 1u);
}

TEST(PaperExecutionEngine, MarketOrderSweepsAvailableDepth) {
    OrderBook book(Symbol("AAPL"));
    book.apply(MarketEvent::make_quote(Timestamp(1), 1, Symbol("AAPL"), Side::Sell,
                                        Price::from_double(100.00), 10));
    book.apply(MarketEvent::make_quote(Timestamp(2), 2, Symbol("AAPL"), Side::Sell,
                                        Price::from_double(100.05), 10));

    PaperExecutionEngine engine;
    auto report = engine.submit_market(1, Symbol("AAPL"), Side::Buy, 15, book, Timestamp(3));
    EXPECT_EQ(report.status, OrderStatus::Filled);
    EXPECT_EQ(report.fill_quantity, 15);
    // volume-weighted avg price across 10@100.00 and 5@100.05
    double expected = (10 * 100.00 + 5 * 100.05) / 15.0;
    EXPECT_NEAR(report.fill_price.to_double(), expected, 1e-4);
}

TEST(PaperExecutionEngine, MarketOrderPartiallyFillsWhenDepthInsufficient) {
    OrderBook book(Symbol("AAPL"));
    book.apply(MarketEvent::make_quote(Timestamp(1), 1, Symbol("AAPL"), Side::Sell,
                                        Price::from_double(100.00), 5));

    PaperExecutionEngine engine;
    auto report = engine.submit_market(1, Symbol("AAPL"), Side::Buy, 20, book, Timestamp(2));
    EXPECT_EQ(report.status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(report.fill_quantity, 5);
}

TEST(PaperExecutionEngine, MarketOrderRejectedWhenNoLiquidity) {
    OrderBook book(Symbol("AAPL"));
    PaperExecutionEngine engine;
    auto report = engine.submit_market(1, Symbol("AAPL"), Side::Buy, 10, book, Timestamp(1));
    EXPECT_EQ(report.status, OrderStatus::Rejected);
    EXPECT_EQ(report.fill_quantity, 0);
    EXPECT_FALSE(report.reason.empty());
}

TEST(PaperExecutionEngine, SameSeedProducesIdenticalFillSequence) {
    FillAssumptions fa;
    fa.rng_seed = 777;
    auto run = [&]() {
        PaperExecutionEngine engine(fa);
        engine.submit_limit(1, Symbol("AAPL"), Side::Buy, Price::from_double(100.00), 100, Timestamp(1));
        std::vector<Quantity> fills;
        for (int i = 0; i < 5; ++i) {
            auto reports = engine.on_market_event(
                trade(static_cast<SequenceNumber>(i + 2), Side::Sell, 99.99, 30, Timestamp(i + 2)));
            for (auto& r : reports) fills.push_back(r.fill_quantity);
        }
        return fills;
    };
    EXPECT_EQ(run(), run());
}
