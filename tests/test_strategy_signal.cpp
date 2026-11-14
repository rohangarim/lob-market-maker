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
    c.strategy.post_only = false;   // opt out so the skewed bid is allowed to cross
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

// ---- tick grid and post-only ------------------------------------------------

namespace {
StrategyConfig tick_cfg() {
    StrategyConfig c;
    c.volatility_spread_multiplier = 0.0;
    c.inventory_skew_bps_per_unit = 0.0;
    c.base_half_spread_bps = 3.1;    // $0.0310 at mid 100: off the grid on purpose
    c.tick_size = 0.01;
    return c;
}
bool on_grid(Price p) { return p.ticks() % 100 == 0; }   // $0.01 == 100 internal ticks
}  // namespace

TEST(TickGrid, QuotesAreRoundedAwayFromMidOntoTheGrid) {
    auto b = book_with(500, 500);   // mid 100.00
    auto d = decide(tick_cfg(), b);
    ASSERT_TRUE(d.quote_bid && d.quote_ask);
    EXPECT_TRUE(on_grid(d.bid_price));
    EXPECT_TRUE(on_grid(d.ask_price));
    EXPECT_DOUBLE_EQ(d.bid_price.to_double(), 99.96);    // 99.969 rounds DOWN
    EXPECT_DOUBLE_EQ(d.ask_price.to_double(), 100.04);   // 100.031 rounds UP
}

TEST(TickGrid, AlreadyOnGridPricesAreNotMovedAnExtraTick) {
    auto b = book_with(500, 500);
    StrategyConfig c = tick_cfg();
    c.base_half_spread_bps = 5.0;    // exactly $0.05 at mid 100
    auto d = decide(c, b);
    EXPECT_DOUBLE_EQ(d.bid_price.to_double(), 99.95);
    EXPECT_DOUBLE_EQ(d.ask_price.to_double(), 100.05);
}

TEST(TickGrid, ZeroTickSizeDisablesRoundingAndClamp) {
    auto b = book_with(500, 500);
    StrategyConfig c = tick_cfg();
    c.tick_size = 0.0;
    auto d = decide(c, b);
    EXPECT_FALSE(on_grid(d.bid_price));
    EXPECT_NEAR(d.bid_price.to_double(), 99.969, 1e-4);
}

TEST(PostOnly, HugeInventorySkewCannotPushQuotesThroughTheTouch) {
    auto b = book_with(500, 500);            // best bid 99.95, best ask 100.05
    StrategyConfig c = tick_cfg();
    c.inventory_skew_bps_per_unit = 5.0;
    c.max_inventory_for_skew = 200;
    MicrostructureCalculator micro;
    MarketMakerStrategy strat(c);
    for (Quantity inv : {-150, -50, 50, 150}) {
        auto d = strat.decide(b, micro, inv, Timestamp(std::chrono::seconds(1)));
        if (d.quote_bid) EXPECT_LE(d.bid_price.to_double(), 100.05 - 0.01 + 1e-9) << "inv=" << inv;
        if (d.quote_ask) EXPECT_GE(d.ask_price.to_double(), 99.95 + 0.01 - 1e-9) << "inv=" << inv;
        if (d.quote_bid && d.quote_ask) EXPECT_LT(d.bid_price, d.ask_price) << "inv=" << inv;
    }
}

TEST(PostOnly, WithoutItTheSameSkewDoesCross) {
    auto b = book_with(500, 500);
    StrategyConfig c = tick_cfg();
    c.inventory_skew_bps_per_unit = 5.0;
    c.post_only = false;
    MicrostructureCalculator micro;
    auto d = MarketMakerStrategy(c).decide(b, micro, -150, Timestamp(std::chrono::seconds(1)));
    ASSERT_TRUE(d.quote_bid);
    EXPECT_GE(d.bid_price.to_double(), 100.05);   // short inventory: bid pushed up through the ask
}

TEST(PostOnly, LongReplayWithAggressiveSkewNeverProducesATakerFill) {
    PipelineConfig c;
    c.strategy.base_half_spread_bps = 0.3;
    c.strategy.inventory_skew_bps_per_unit = 0.5;     // the setting that used to force crossing
    c.strategy.imbalance_skew_bps = 30.0;
    c.risk_limits.max_daily_loss = 1e9;
    Pipeline p(Symbol("AAPL"), c);
    SyntheticBookGenerator g(21, 190.0, 0.01, Symbol("AAPL"), 0.9);
    for (int i = 0; i < 400000; ++i) p.process(g.next());
    EXPECT_GT(p.stats().fills, 100u);
    EXPECT_EQ(p.execution().total_taker_fills(), 0u);
}

TEST(TickGrid, EveryQuoteInALongReplayLandsOnTheGrid) {
    PipelineConfig c;
    c.strategy.base_half_spread_bps = 2.3;
    c.strategy.volatility_spread_multiplier = 3.0;
    c.risk_limits.max_daily_loss = 1e9;
    Pipeline p(Symbol("AAPL"), c);
    SyntheticBookGenerator g(4, 190.0, 0.01, Symbol("AAPL"));
    size_t checked = 0;
    for (int i = 0; i < 100000; ++i) {
        p.process(g.next());
        for (const auto& o : p.execution().open_orders()) {
            ASSERT_TRUE(on_grid(o.price)) << o.price.ticks();
            ++checked;
        }
    }
    EXPECT_GT(checked, 1000u);
}

TEST(InformedFlow, NextMoveFollowsTheLastTradeOnlyWhenEnabled) {
    auto aligned = [](double informed) {
        SyntheticBookGenerator g(31, 190.0, 0.01, Symbol("AAPL"), 0.0, informed);
        size_t last = 0, n = 0, agree = 0;
        for (int i = 0; i < 600000; ++i) {
            g.next();
            if (g.moves() != last) {
                last = g.moves();
                if (g.last_move_flow() != 0.0) {
                    ++n;
                    if ((g.last_move_flow() > 0) == g.last_move_up()) ++agree;
                }
            }
        }
        EXPECT_GT(n, 2000u);
        return static_cast<double>(agree) / static_cast<double>(n);
    };
    EXPECT_NEAR(aligned(0.0), 0.5, 0.05);
    EXPECT_GT(aligned(0.9), 0.68);
}

TEST(InformedFlow, ZeroKeepsTheStreamIdenticalToTheDefault) {
    SyntheticBookGenerator a(8, 190.0, 0.01, Symbol("AAPL"), 0.3);
    SyntheticBookGenerator b(8, 190.0, 0.01, Symbol("AAPL"), 0.3, 0.0);
    for (int i = 0; i < 20000; ++i) {
        auto x = a.next(), y = b.next();
        ASSERT_EQ(x.type, y.type);
        ASSERT_EQ(x.price.ticks(), y.price.ticks());
    }
}

TEST(InformedFlow, MakerFillsAreAdverselySelectedOnlyWhenFlowIsInformed) {
    auto markout = [](double informed) {
        PipelineConfig c;
        c.strategy.base_half_spread_bps = 0.5;
        c.strategy.inventory_skew_bps_per_unit = 0.01;
        c.risk_limits.max_daily_loss = 1e9;
        c.markout_horizons = {std::chrono::milliseconds(50)};
        Pipeline p(Symbol("AAPL"), c);
        SyntheticBookGenerator g(2, 190.0, 0.01, Symbol("AAPL"), 0.0, informed);
        for (int i = 0; i < 400000; ++i) p.process(g.next());
        EXPECT_GT(p.markout().fill_count(), 500u);
        return p.markout().avg_markout(0) - p.markout().avg_edge();   // negative = mid moved against us
    };
    double uninformed = markout(0.0);
    double informed = markout(0.9);
    EXPECT_LT(informed, uninformed - 0.002);
}

// ---- fill invariants (permanent version of a one-off audit) ----------------

TEST(FillInvariants, EveryMakerFillIsTriggeredByATradeAtOrThroughItsPrice) {
    struct Case { FillModel model; int64_t order_us; int64_t cancel_us; };
    for (Case cs : {Case{FillModel::QueuePosition, 0, 0}, Case{FillModel::QueuePosition, 300, 700},
                    Case{FillModel::TradeThrough, 0, 0}}) {
        PipelineConfig c;
        c.strategy.base_half_spread_bps = 0.5;
        c.strategy.inventory_skew_bps_per_unit = 0.01;
        c.risk_limits.max_daily_loss = 1e9;
        c.fill_assumptions.fill_model = cs.model;
        c.fill_assumptions.order_latency = std::chrono::microseconds(cs.order_us);
        c.fill_assumptions.cancel_latency = std::chrono::microseconds(cs.cancel_us);
        Pipeline p(Symbol("AAPL"), c);
        SyntheticBookGenerator g(1234, 190.0, 0.01, Symbol("AAPL"), 0.0, 0.8);

        MarketEvent cur{};
        size_t fills = 0, violations = 0, off_grid = 0;
        Quantity max_pos = 0;
        p.set_fill_observer([&](const ExecutionReport& r, std::optional<double>) {
            ++fills;
            if (r.taker) { ++violations; return; }                       // post-only: never a taker
            if (cur.type != EventType::Trade) { ++violations; return; }  // maker fills come from prints only
            bool ok = r.side == Side::Buy ? cur.price <= r.fill_price : cur.price >= r.fill_price;
            if (!ok) ++violations;
            if (r.fill_price.ticks() % 100 != 0) ++off_grid;
            if (r.fill_quantity <= 0) ++violations;
        });
        for (int i = 0; i < 250000; ++i) {
            cur = g.next();
            p.process(cur);
            max_pos = std::max<Quantity>(max_pos, std::abs(p.portfolio().state().position));
        }
        EXPECT_GT(fills, 1000u);
        EXPECT_EQ(violations, 0u);
        EXPECT_EQ(off_grid, 0u);
        EXPECT_LE(max_pos, c.risk_limits.max_position);
    }
}
