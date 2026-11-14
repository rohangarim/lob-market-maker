#include <gtest/gtest.h>

#include "common/pipeline.hpp"
#include "orderbook/order_book.hpp"
#include "portfolio/portfolio.hpp"
#include "strategy/market_maker.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;

namespace {

OrderBook book_with(Quantity bid_qty, Quantity ask_qty) {
    OrderBook b(Symbol("AAPL"));
    Timestamp t(std::chrono::seconds(1));
    b.apply(MarketEvent::make_quote(t, 1, Symbol("AAPL"), Side::Buy, Price::from_double(99.95), bid_qty));
    b.apply(MarketEvent::make_quote(t, 2, Symbol("AAPL"), Side::Sell, Price::from_double(100.05), ask_qty));
    return b;
}

StrategyConfig base_cfg() {
    StrategyConfig c;
    c.volatility_spread_multiplier = 0.0;
    c.inventory_skew_bps_per_unit = 0.0;
    return c;
}

QuoteDecision decide(const StrategyConfig& c, const OrderBook& b) {
    MicrostructureCalculator micro;
    return MarketMakerStrategy(c).decide(b, micro, 0, Timestamp(std::chrono::seconds(1)));
}

}  // namespace

TEST(StrategySignal, DisabledByDefaultMatchesPlainMid) {
    auto b = book_with(900, 100);
    auto plain = decide(base_cfg(), b);
    StrategyConfig c = base_cfg();
    c.imbalance_skew_bps = 0.0;
    c.use_microprice = false;
    auto same = decide(c, b);
    EXPECT_EQ(plain.bid_price, same.bid_price);
    EXPECT_EQ(plain.ask_price, same.ask_price);
}

TEST(StrategySignal, MicropriceReferenceLeansTowardTheHeavierBid) {
    auto b = book_with(900, 100);   // bid-heavy: microprice sits above mid
    auto plain = decide(base_cfg(), b);
    StrategyConfig c = base_cfg();
    c.use_microprice = true;
    auto mp = decide(c, b);
    EXPECT_GT(mp.bid_price.to_double(), plain.bid_price.to_double());
    EXPECT_GT(mp.ask_price.to_double(), plain.ask_price.to_double());
    EXPECT_NEAR(mp.ask_price.to_double() - mp.bid_price.to_double(),
                plain.ask_price.to_double() - plain.bid_price.to_double(), 1e-6);  // spread unchanged
}

TEST(StrategySignal, MicropriceEqualsMidOnABalancedBook) {
    auto b = book_with(500, 500);
    StrategyConfig c = base_cfg();
    c.use_microprice = true;
    auto mp = decide(c, b);
    auto plain = decide(base_cfg(), b);
    EXPECT_EQ(mp.bid_price, plain.bid_price);
}

TEST(StrategySignal, ImbalanceSkewShiftsReferenceByExactlyTheFormula) {
    auto b = book_with(300, 100);   // OBI = (300-100)/400 = 0.5
    StrategyConfig c = base_cfg();
    c.imbalance_skew_bps = 10.0;    // 10 bps * 0.5 * mid(100.00) = $0.05
    auto skewed = decide(c, b);
    auto plain = decide(base_cfg(), b);
    EXPECT_NEAR(skewed.bid_price.to_double() - plain.bid_price.to_double(), 0.05, 1e-4);
    EXPECT_NEAR(skewed.ask_price.to_double() - plain.ask_price.to_double(), 0.05, 1e-4);
}

TEST(StrategySignal, AskHeavyBookShiftsQuotesDown) {
    auto b = book_with(100, 300);
    StrategyConfig c = base_cfg();
    c.imbalance_skew_bps = 10.0;
    EXPECT_LT(decide(c, b).bid_price.to_double(), decide(base_cfg(), b).bid_price.to_double());
}

TEST(StrategySignal, BalancedBookHasNoImbalanceShift) {
    auto b = book_with(250, 250);
    StrategyConfig c = base_cfg();
    c.imbalance_skew_bps = 25.0;
    EXPECT_EQ(decide(c, b).bid_price, decide(base_cfg(), b).bid_price);
}

TEST(StrategySignal, ImbalanceUsesOnlyTheConfiguredNumberOfLevels) {
    OrderBook b(Symbol("AAPL"));
    Timestamp t(std::chrono::seconds(1));
    auto q = [&](Side s, double p, Quantity n, SequenceNumber seq) {
        b.apply(MarketEvent::make_quote(t, seq, Symbol("AAPL"), s, Price::from_double(p), n));
    };
    q(Side::Buy, 99.95, 100, 1);  q(Side::Sell, 100.05, 100, 2);   // balanced at the touch
    q(Side::Buy, 99.94, 10000, 3);                                  // huge size on bid level 2
    StrategyConfig one = base_cfg();
    one.imbalance_skew_bps = 10.0;
    one.imbalance_levels = 1;
    StrategyConfig two = one;
    two.imbalance_levels = 2;
    EXPECT_EQ(decide(one, b).bid_price, decide(base_cfg(), b).bid_price);   // level 2 ignored
    EXPECT_GT(decide(two, b).bid_price.to_double(), decide(base_cfg(), b).bid_price.to_double());
}

TEST(StrategySignal, QuotesNeverCrossAfterAnExtremeImbalanceShift) {
    auto b = book_with(100000, 1);
    StrategyConfig c = base_cfg();
    c.use_microprice = true;
    c.imbalance_skew_bps = 40.0;
    auto d = decide(c, b);
    ASSERT_TRUE(d.quote_bid && d.quote_ask);
    EXPECT_LT(d.bid_price, d.ask_price);
}

// ---- planted-signal generator ----------------------------------------------

TEST(PlantedSignal, ReferenceMovesFollowImbalanceOnlyWhenStrengthIsSet) {
    auto aligned_fraction = [](double strength) {
        SyntheticBookGenerator g(77, 190.0, 0.01, Symbol("AAPL"), strength);
        size_t last_moves = 0, aligned = 0, counted = 0;
        for (int i = 0; i < 600000; ++i) {
            g.next();
            if (g.moves() != last_moves) {
                last_moves = g.moves();
                double oi = g.last_move_imbalance();
                if (std::abs(oi) > 0.2) {
                    ++counted;
                    if ((oi > 0) == g.last_move_up()) ++aligned;
                }
            }
        }
        EXPECT_GT(counted, 500u);
        return static_cast<double>(aligned) / static_cast<double>(counted);
    };
    EXPECT_NEAR(aligned_fraction(0.0), 0.5, 0.06);
    EXPECT_GT(aligned_fraction(0.9), 0.62);
}

TEST(PlantedSignal, StrengthZeroKeepsTheStreamIdenticalToTheDefault) {
    SyntheticBookGenerator a(5, 190.0, 0.01, Symbol("AAPL"));
    SyntheticBookGenerator b(5, 190.0, 0.01, Symbol("AAPL"), 0.0);
    for (int i = 0; i < 20000; ++i) {
        auto x = a.next(), y = b.next();
        ASSERT_EQ(x.type, y.type);
        ASSERT_EQ(x.price.ticks(), y.price.ticks());
        ASSERT_EQ(x.quantity, y.quantity);
    }
}

// ---- portfolio fees and order-book helpers --------------------------------

TEST(PortfolioFees, FeesReduceTotalPnlAndRebatesIncreaseIt) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.0), 10, /*fee=*/0.03);
    p.mark_to_market(100.0);
    EXPECT_NEAR(p.state().total_pnl, -0.03, 1e-12);
    p.on_fill(Side::Sell, Price::from_double(100.0), 10, /*fee=*/-0.02);  // rebate
    EXPECT_NEAR(p.state().fees_paid, 0.01, 1e-12);
    EXPECT_NEAR(p.state().total_pnl, -0.01, 1e-12);
}

TEST(PortfolioFees, TotalPnlStaysConsistentBetweenMarks) {
    Portfolio p;
    p.mark_to_market(100.0);
    p.on_fill(Side::Buy, Price::from_double(99.0), 10);   // bought 1.00 under the mark
    EXPECT_NEAR(p.state().unrealized_pnl, 10.0, 1e-9);
    EXPECT_NEAR(p.state().total_pnl, 10.0, 1e-9);
}

TEST(OrderBookViews, QuantityAtFindsExactLevelsOnly) {
    auto b = book_with(300, 100);
    EXPECT_EQ(b.quantity_at(Side::Buy, Price::from_double(99.95)), 300);
    EXPECT_EQ(b.quantity_at(Side::Sell, Price::from_double(100.05)), 100);
    EXPECT_EQ(b.quantity_at(Side::Buy, Price::from_double(99.94)), 0);
    EXPECT_EQ(b.quantity_at(Side::Sell, Price::from_double(99.95)), 0);  // right price, wrong side
}

TEST(OrderBookViews, DepthSpansAreBoundedAndOrdered) {
    OrderBook b(Symbol("AAPL"));
    Timestamp t(std::chrono::seconds(1));
    for (int i = 0; i < 5; ++i)
        b.apply(MarketEvent::make_quote(t, i + 1, Symbol("AAPL"), Side::Buy, Price::from_double(100 - i * 0.01), 10 + i));
    EXPECT_EQ(b.bid_depth(3).size(), 3u);
    EXPECT_EQ(b.bid_depth(99).size(), 5u);
    EXPECT_EQ(b.ask_depth(3).size(), 0u);
    EXPECT_GT(b.bid_depth(5)[0].price, b.bid_depth(5)[4].price);
    EXPECT_EQ(b.bids().size(), 5u);
}

// ---- pipeline-level behaviour of the new pieces ----------------------------

TEST(PipelineExecution, MarketableQuoteFromSkewIsFilledAsTakerAndCounted) {
    PipelineConfig c;
    c.strategy.base_half_spread_bps = 1.0;
    c.strategy.inventory_skew_bps_per_unit = 0.0;
    c.strategy.volatility_spread_multiplier = 0.0;
    c.strategy.imbalance_skew_bps = 60.0;   // pushes the ask below the best bid when bid-heavy
    c.strategy.imbalance_levels = 1;
    c.fill_assumptions.taker_fee_per_share = 0.003;
    Pipeline p(Symbol("AAPL"), c);
    Timestamp t(std::chrono::seconds(1));
    p.process(MarketEvent::make_quote(t, 1, Symbol("AAPL"), Side::Buy, Price::from_double(99.99), 1000));
    p.process(MarketEvent::make_quote(t, 2, Symbol("AAPL"), Side::Sell, Price::from_double(100.01), 10));
    // OBI ~ +0.98 -> reference ~ +59bps = ~$0.59 above mid, so the BID crosses the ask and takes it.
    EXPECT_GE(p.execution().total_taker_fills(), 1u);
    EXPECT_GT(p.portfolio().state().position, 0);
    EXPECT_GT(p.portfolio().state().fees_paid, 0.0);
}

TEST(PipelineAnalytics, MarkoutAndDrawdownArePopulatedByAReplay) {
    PipelineConfig c;
    c.fill_assumptions.partial_fill_probability = 0.0;
    Pipeline p(Symbol("AAPL"), c);
    SyntheticBookGenerator g(3, 190.0, 0.01, Symbol("AAPL"));
    for (int i = 0; i < 300000; ++i) p.process(g.next());
    EXPECT_GT(p.markout().fill_count(), 0u);
    EXPECT_GT(p.analytics().samples, 0u);
    EXPECT_GE(p.analytics().max_drawdown, 0.0);
    EXPECT_LE(p.analytics().max_abs_position, c.risk_limits.max_position);
}
