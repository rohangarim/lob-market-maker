#include <gtest/gtest.h>

#include "execution/paper_execution_engine.hpp"
#include "orderbook/order_book.hpp"

using namespace lob;

namespace {

Timestamp us(int64_t v) { return std::chrono::microseconds(v); }
Price px(double d) { return Price::from_double(d); }

MarketEvent quote(Side side, double p, Quantity q, Timestamp t) {
    return MarketEvent::make_quote(t, 0, Symbol("AAPL"), side, px(p), q);
}
MarketEvent del(Side side, double p, Timestamp t) {
    return MarketEvent::make_delete(t, 0, Symbol("AAPL"), side, px(p));
}
MarketEvent trade(double p, Quantity q, Timestamp t) {
    return MarketEvent::make_trade(t, 0, Symbol("AAPL"), Side::Sell, px(p), q);
}

// Book with 100 shares bid at 100.00 and 50 offered at 100.10.
OrderBook make_book() {
    OrderBook b(Symbol("AAPL"));
    b.apply(quote(Side::Buy, 100.00, 100, us(0)));
    b.apply(quote(Side::Sell, 100.10, 50, us(0)));
    return b;
}

FillAssumptions queue_cfg() {
    FillAssumptions fa;
    fa.fill_model = FillModel::QueuePosition;
    return fa;
}

}  // namespace

TEST(QueueModel, JoinsBackOfQueueBehindVisibleQuantity) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    auto o = e.get_order(1);
    ASSERT_TRUE(o);
    EXPECT_DOUBLE_EQ(o->queue_ahead, 100.0);
}

TEST(QueueModel, TradeAtPriceSmallerThanQueueDoesNotFill) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    auto r = e.on_market_event(trade(100.00, 60, us(2)));
    EXPECT_TRUE(r.empty());
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 40.0);
}

TEST(QueueModel, OnlyExcessBeyondQueueFills) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    EXPECT_TRUE(e.on_market_event(trade(100.00, 60, us(2))).empty());
    auto r = e.on_market_event(trade(100.00, 50, us(3)));  // 40 left ahead, 10 spill to us
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 10);
    EXPECT_EQ(r[0].status, OrderStatus::Filled);
    EXPECT_DOUBLE_EQ(r[0].fill_price.to_double(), 100.00);
    EXPECT_FALSE(r[0].taker);
}

TEST(QueueModel, ExcessSmallerThanOrderIsAPartialFill) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 30, us(1), book);
    auto r = e.on_market_event(trade(100.00, 112, us(2)));  // 100 ahead, 12 spill
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 12);
    EXPECT_EQ(r[0].status, OrderStatus::PartiallyFilled);
    EXPECT_EQ(r[0].remaining_quantity, 18);
    EXPECT_EQ(e.open_order_count(), 1u);
}

TEST(QueueModel, TradeThroughThePriceFillsTheWholeRemainder) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 30, us(1), book);
    auto r = e.on_market_event(trade(99.99, 1, us(2)));  // level exhausted, so we are filled
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 30);
    EXPECT_EQ(e.open_order_count(), 0u);
}

TEST(QueueModel, SellSideMirror) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Sell, px(100.10), 20, us(1), book);
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 50.0);
    EXPECT_TRUE(e.on_market_event(trade(100.10, 50, us(2))).empty());
    auto r = e.on_market_event(trade(100.10, 5, us(3)));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 5);
    auto through = e.on_market_event(trade(100.11, 1, us(4)));
    ASSERT_EQ(through.size(), 1u);
    EXPECT_EQ(through[0].fill_quantity, 15);
}

TEST(QueueModel, TradeAwayFromOurPriceChangesNothing) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(99.90), 10, us(1), book);  // empty level, behind the touch
    EXPECT_TRUE(e.on_market_event(trade(100.00, 500, us(2))).empty());  // trades at a better price
    EXPECT_EQ(e.open_order_count(), 1u);
}

TEST(QueueModel, EmptyLevelMeansNothingAheadSoFirstTradeAtPriceFills) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(99.90), 10, us(1), book);
    auto r = e.on_market_event(trade(99.90, 4, us(2)));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 4);
}

TEST(QueueModel, CancellationsShrinkTheQueueProRata) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    // Level drops 100 -> 50 with no trades: half of the queue is assumed canceled
    // uniformly, so half of what was ahead of us goes away.
    e.on_market_event(quote(Side::Buy, 100.00, 50, us(2)));
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 50.0);
}

TEST(QueueModel, ShrinkExplainedByTradesIsNotDoubleCounted) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    e.on_market_event(trade(100.00, 30, us(2)));                  // queue 100 -> 70
    e.on_market_event(quote(Side::Buy, 100.00, 70, us(3)));       // feed reports the same 30 gone
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 70.0);
}

TEST(QueueModel, CancelsBeyondTradesShrinkOnlyTheUnexplainedPart) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    e.on_market_event(trade(100.00, 20, us(2)));                  // queue 80, expected level 80
    e.on_market_event(quote(Side::Buy, 100.00, 40, us(3)));       // 40 more vanished, 80 -> 40
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 40.0);
}

TEST(QueueModel, SizeIncreasesJoinBehindUs) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    e.on_market_event(quote(Side::Buy, 100.00, 400, us(2)));
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 100.0);
}

TEST(QueueModel, DeletedLevelClearsTheQueue) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    e.on_market_event(del(Side::Buy, 100.00, us(2)));
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 0.0);
}

TEST(QueueModel, UpdatesOnTheOtherSideOrPriceAreIgnored) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1), book);
    e.on_market_event(quote(Side::Sell, 100.00, 5, us(2)));
    e.on_market_event(quote(Side::Buy, 99.99, 5, us(3)));
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 100.0);
}

TEST(QueueModel, QueueModelIsFullyDeterministicWithoutRng) {
    auto run = [](uint64_t seed) {
        auto book = make_book();
        FillAssumptions fa = queue_cfg();
        fa.rng_seed = seed;
        PaperExecutionEngine e(fa);
        e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 50, us(1), book);
        std::vector<Quantity> fills;
        for (int i = 0; i < 6; ++i)
            for (auto& r : e.on_market_event(trade(100.00, 30, us(2 + i)))) fills.push_back(r.fill_quantity);
        return fills;
    };
    EXPECT_EQ(run(1), run(999));
    EXPECT_EQ(run(1), (std::vector<Quantity>{20, 30}));  // 100 ahead: 30,30,30 -> 10 left; then 20 spill, then 30
}

// ---- latency -------------------------------------------------------------

TEST(ExecutionLatency, OrderCannotFillBeforeItIsLive) {
    auto book = make_book();
    FillAssumptions fa = queue_cfg();
    fa.order_latency = us(500);
    PaperExecutionEngine e(fa);
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(99.90), 10, us(1000), book);
    EXPECT_TRUE(e.on_market_event(trade(99.89, 10, us(1200))).empty());  // 200 us after submit: not live
    auto r = e.on_market_event(trade(99.89, 10, us(1600)));              // live
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 10);
}

TEST(ExecutionLatency, QueueAheadIsStillConsumedBeforeTheOrderIsLive) {
    auto book = make_book();
    FillAssumptions fa = queue_cfg();
    fa.order_latency = us(500);
    PaperExecutionEngine e(fa);
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(1000), book);
    EXPECT_TRUE(e.on_market_event(trade(100.00, 100, us(1100))).empty());  // clears the 100 ahead, not live
    EXPECT_DOUBLE_EQ(e.get_order(1)->queue_ahead, 0.0);
    auto r = e.on_market_event(trade(100.00, 10, us(1700)));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].fill_quantity, 10);
}

TEST(ExecutionLatency, ZeroCancelLatencyCancelsImmediately) {
    PaperExecutionEngine e(queue_cfg());
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(99.90), 10, us(1));
    auto r = e.cancel(1, us(2));
    ASSERT_TRUE(r);
    EXPECT_EQ(r->status, OrderStatus::Canceled);
    EXPECT_EQ(e.open_order_count(), 0u);
}

TEST(ExecutionLatency, OrderCanStillFillWhileCancelIsInFlight) {
    FillAssumptions fa = queue_cfg();
    fa.cancel_latency = us(1000);
    PaperExecutionEngine e(fa);
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(0));
    EXPECT_FALSE(e.cancel(1, us(100)).has_value());          // request accepted, not yet effective
    EXPECT_EQ(e.open_order_count(), 1u);
    auto r = e.on_market_event(trade(99.99, 1, us(600)));    // race: fills before the cancel lands
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].status, OrderStatus::Filled);
    EXPECT_EQ(e.total_canceled(), 0u);
}

TEST(ExecutionLatency, CancelTakesEffectOnceItsTimeArrives) {
    FillAssumptions fa = queue_cfg();
    fa.cancel_latency = us(1000);
    PaperExecutionEngine e(fa);
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(0));
    e.cancel(1, us(100));
    auto r = e.on_market_event(trade(99.99, 1, us(1100)));   // exactly at effective time: cancel wins
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].status, OrderStatus::Canceled);
    EXPECT_EQ(e.open_order_count(), 0u);
    EXPECT_EQ(e.total_canceled(), 1u);
}

TEST(ExecutionLatency, DoubleCancelRequestIsIgnored) {
    FillAssumptions fa = queue_cfg();
    fa.cancel_latency = us(1000);
    PaperExecutionEngine e(fa);
    e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 10, us(0));
    e.cancel(1, us(100));
    EXPECT_FALSE(e.cancel(1, us(200)).has_value());
}

// ---- marketable limits and fees -------------------------------------------

TEST(MarketableLimit, BuyAboveTheAskTakesLiquidityFirst) {
    auto book = make_book();  // 50 @ 100.10 offered
    PaperExecutionEngine e(queue_cfg());
    auto res = e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.20), 20, us(1), book);
    ASSERT_TRUE(res.taker_fill);
    EXPECT_EQ(res.taker_fill->fill_quantity, 20);
    EXPECT_DOUBLE_EQ(res.taker_fill->fill_price.to_double(), 100.10);  // pays the ask, not its own limit
    EXPECT_TRUE(res.taker_fill->taker);
    EXPECT_EQ(e.open_order_count(), 0u);
    EXPECT_EQ(e.total_taker_fills(), 1u);
}

TEST(MarketableLimit, RemainderRestsAfterSweepingAvailableDepth) {
    OrderBook book(Symbol("AAPL"));
    book.apply(quote(Side::Buy, 100.00, 100, us(0)));
    book.apply(quote(Side::Sell, 100.10, 10, us(0)));
    book.apply(quote(Side::Sell, 100.20, 10, us(0)));
    PaperExecutionEngine e(queue_cfg());
    auto res = e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.15), 25, us(1), book);
    ASSERT_TRUE(res.taker_fill);
    EXPECT_EQ(res.taker_fill->fill_quantity, 10);  // only the 100.10 level is within the limit
    EXPECT_EQ(res.taker_fill->status, OrderStatus::PartiallyFilled);
    auto resting = e.get_order(1);
    ASSERT_TRUE(resting);
    EXPECT_EQ(resting->remaining_quantity(), 15);
}

TEST(MarketableLimit, PassiveOrderBehindTheTouchTakesNothing) {
    auto book = make_book();
    PaperExecutionEngine e(queue_cfg());
    auto res = e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.00), 20, us(1), book);
    EXPECT_FALSE(res.taker_fill);
    EXPECT_EQ(res.ack.status, OrderStatus::New);
    EXPECT_EQ(e.open_order_count(), 1u);
}

TEST(Fees, MakerRebateAndTakerFeeAreChargedPerShare) {
    auto book = make_book();
    FillAssumptions fa = queue_cfg();
    fa.maker_fee_per_share = -0.002;
    fa.taker_fee_per_share = 0.003;
    PaperExecutionEngine e(fa);

    auto taker = e.submit_limit(1, Symbol("AAPL"), Side::Buy, px(100.20), 20, us(1), book);
    EXPECT_NEAR(taker.taker_fill->fee, 0.003 * 20, 1e-12);

    e.submit_limit(2, Symbol("AAPL"), Side::Buy, px(99.90), 10, us(2), book);
    auto r = e.on_market_event(trade(99.90, 10, us(3)));
    ASSERT_EQ(r.size(), 1u);
    EXPECT_NEAR(r[0].fee, -0.002 * 10, 1e-12);
    EXPECT_FALSE(r[0].taker);
}

TEST(Fees, MarketOrderPaysTakerFee) {
    auto book = make_book();
    FillAssumptions fa = queue_cfg();
    fa.taker_fee_per_share = 0.003;
    PaperExecutionEngine e(fa);
    auto r = e.submit_market(1, Symbol("AAPL"), Side::Buy, 10, book, us(1));
    EXPECT_NEAR(r.fee, 0.03, 1e-12);
    EXPECT_TRUE(r.taker);
}
