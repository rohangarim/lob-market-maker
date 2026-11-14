#include <gtest/gtest.h>

#include "common/pipeline.hpp"
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

// ---- O(1) rolling volatility must agree with a direct computation ------------

namespace {
double naive_vol(const std::vector<double>& mids, size_t window) {
    std::vector<double> rets;
    for (size_t i = 1; i < mids.size(); ++i) rets.push_back(std::log(mids[i] / mids[i - 1]));
    if (rets.size() > window) rets.erase(rets.begin(), rets.end() - static_cast<long>(window));
    if (rets.size() < 2) return -1.0;
    double mean = 0;
    for (double r : rets) mean += r;
    mean /= static_cast<double>(rets.size());
    double var = 0;
    for (double r : rets) var += (r - mean) * (r - mean);
    return std::sqrt(var / static_cast<double>(rets.size() - 1));
}
}  // namespace

TEST(Microstructure, RunningVolatilityMatchesDirectComputationThroughWindowWrapAndRecompute) {
    constexpr size_t kWindow = 50;
    MicrostructureCalculator micro(kWindow);
    std::vector<double> mids;
    OrderBook book(Symbol("AAPL"));
    double px = 190.0;
    unsigned s = 12345;
    // 20,000 updates crosses the exact-recompute boundary (4096) several times.
    for (int i = 0; i < 20000; ++i) {
        s = s * 1664525u + 1013904223u;
        px += (static_cast<double>(s >> 8) / 16777216.0 - 0.5) * 0.04;
        // One level per side, replaced every update.
        book.clear();
        book.apply(MarketEvent::make_quote(Timestamp(i), 1, Symbol("AAPL"), Side::Buy, Price::from_double(px - 0.01), 10));
        book.apply(MarketEvent::make_quote(Timestamp(i), 2, Symbol("AAPL"), Side::Sell, Price::from_double(px + 0.01), 10));
        mids.push_back(*book.mid_price());
        micro.on_book_update(book, Timestamp(i));
        if (mids.size() > kWindow + 2 && i % 997 == 0) {
            double expect = naive_vol(std::vector<double>(mids.end() - static_cast<long>(kWindow) - 1, mids.end()), kWindow);
            auto got = micro.short_term_volatility();
            ASSERT_TRUE(got.has_value());
            EXPECT_NEAR(*got, expect, 1e-9 * std::max(1.0, expect)) << "at update " << i;
        }
    }
}

TEST(Microstructure, ResetPriceHistoryClearsTheRunningSums) {
    MicrostructureCalculator micro(10);
    OrderBook book(Symbol("AAPL"));
    for (int i = 0; i < 30; ++i) {
        book.clear();
        book.apply(MarketEvent::make_quote(Timestamp(i), 1, Symbol("AAPL"), Side::Buy, Price::from_double(100.0 + i * 0.3), 5));
        book.apply(MarketEvent::make_quote(Timestamp(i), 2, Symbol("AAPL"), Side::Sell, Price::from_double(100.1 + i * 0.3), 5));
        micro.on_book_update(book, Timestamp(i));
    }
    ASSERT_TRUE(micro.short_term_volatility().has_value());
    micro.reset_price_history();
    EXPECT_FALSE(micro.short_term_volatility().has_value());
    for (int i = 0; i < 3; ++i) {
        book.clear();
        book.apply(MarketEvent::make_quote(Timestamp(i), 1, Symbol("AAPL"), Side::Buy, Price::from_double(200.0), 5));
        book.apply(MarketEvent::make_quote(Timestamp(i), 2, Symbol("AAPL"), Side::Sell, Price::from_double(200.1), 5));
        micro.on_book_update(book, Timestamp(i));
    }
    // Constant mid after the reset: zero volatility, with no memory of the earlier trend.
    ASSERT_TRUE(micro.short_term_volatility().has_value());
    EXPECT_NEAR(*micro.short_term_volatility(), 0.0, 1e-12);
}

TEST(Pipeline, DecisionLatencyIsSampledAtTheConfiguredStride) {
    PipelineConfig c;
    c.latency_sample_interval = 8;
    c.strategy.base_half_spread_bps = 0.5;
    Pipeline p(Symbol("AAPL"), c);
    Timestamp t(std::chrono::seconds(1));
    size_t requotes = 0;
    for (int i = 0; i < 2000; ++i) {
        double bump = (i % 2) ? 0.01 : 0.0;
        p.process(MarketEvent::make_quote(t, 2 * i + 1, Symbol("AAPL"), Side::Buy, Price::from_double(99.95 + bump), 100));
        p.process(MarketEvent::make_quote(t, 2 * i + 2, Symbol("AAPL"), Side::Sell, Price::from_double(100.05 + bump), 100));
        requotes += 2;
    }
    EXPECT_EQ(p.latency_sample_interval(), 8u);
    EXPECT_GT(p.stats().decision_latency.count(), 100u);
    EXPECT_LT(p.stats().decision_latency.count(), requotes / 8 + 2);

    PipelineConfig every = c;
    every.latency_sample_interval = 1;
    Pipeline q(Symbol("AAPL"), every);
    for (int i = 0; i < 500; ++i) {
        q.process(MarketEvent::make_quote(t, 2 * i + 1, Symbol("AAPL"), Side::Buy, Price::from_double(99.95), 100 + i));
        q.process(MarketEvent::make_quote(t, 2 * i + 2, Symbol("AAPL"), Side::Sell, Price::from_double(100.05), 100 + i));
    }
    EXPECT_EQ(q.stats().decision_latency.count(), 1000u);  // requote runs (and is timed) on every book-changing event
}
