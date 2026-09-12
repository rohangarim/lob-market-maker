#include <gtest/gtest.h>

#include "metrics/microstructure.hpp"
#include "orderbook/order_book.hpp"
#include "strategy/market_maker.hpp"

using namespace lob;

namespace {
OrderBook make_book_with_market(double bid_px, double ask_px, Quantity qty, Timestamp ts) {
    OrderBook book(Symbol("AAPL"));
    book.apply(MarketEvent::make_quote(ts, 1, Symbol("AAPL"), Side::Buy, Price::from_double(bid_px), qty));
    book.apply(MarketEvent::make_quote(ts, 2, Symbol("AAPL"), Side::Sell, Price::from_double(ask_px), qty));
    return book;
}
}  // namespace

TEST(MarketMakerStrategy, NoQuoteOnEmptyBook) {
    OrderBook book(Symbol("AAPL"));
    MicrostructureCalculator micro;
    MarketMakerStrategy strategy(StrategyConfig{});
    auto decision = strategy.decide(book, micro, 0, Timestamp(1));
    EXPECT_FALSE(decision.quote_bid);
    EXPECT_FALSE(decision.quote_ask);
}

TEST(MarketMakerStrategy, QuotesSymmetricallyAroundMidWhenFlat) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(1));
    MicrostructureCalculator micro;
    StrategyConfig cfg;
    cfg.base_half_spread_bps = 10.0;
    cfg.inventory_skew_bps_per_unit = 0.0;
    cfg.volatility_spread_multiplier = 0.0;
    MarketMakerStrategy strategy(cfg);

    auto decision = strategy.decide(book, micro, 0, Timestamp(1));
    ASSERT_TRUE(decision.quote_bid);
    ASSERT_TRUE(decision.quote_ask);
    double mid = *book.mid_price();
    double bid_dist = mid - decision.bid_price.to_double();
    double ask_dist = decision.ask_price.to_double() - mid;
    EXPECT_NEAR(bid_dist, ask_dist, 1e-6);
}

TEST(MarketMakerStrategy, LongInventorySkewsQuotesDownToEncourageSelling) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(1));
    MicrostructureCalculator micro;
    StrategyConfig cfg;
    cfg.inventory_skew_bps_per_unit = 5.0;
    cfg.max_inventory_for_skew = 200;
    cfg.volatility_spread_multiplier = 0.0;
    MarketMakerStrategy strategy(cfg);

    auto flat = strategy.decide(book, micro, 0, Timestamp(1));
    auto long_inv = strategy.decide(book, micro, 100, Timestamp(1));

    ASSERT_TRUE(flat.quote_bid && flat.quote_ask);
    ASSERT_TRUE(long_inv.quote_bid && long_inv.quote_ask);
    // Being long should pull both quotes down relative to the flat case.
    EXPECT_LT(long_inv.bid_price.to_double(), flat.bid_price.to_double());
    EXPECT_LT(long_inv.ask_price.to_double(), flat.ask_price.to_double());
}

TEST(MarketMakerStrategy, ShortInventorySkewsQuotesUpToEncourageBuying) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(1));
    MicrostructureCalculator micro;
    StrategyConfig cfg;
    cfg.inventory_skew_bps_per_unit = 5.0;
    cfg.volatility_spread_multiplier = 0.0;
    MarketMakerStrategy strategy(cfg);

    auto flat = strategy.decide(book, micro, 0, Timestamp(1));
    auto short_inv = strategy.decide(book, micro, -100, Timestamp(1));

    EXPECT_GT(short_inv.bid_price.to_double(), flat.bid_price.to_double());
    EXPECT_GT(short_inv.ask_price.to_double(), flat.ask_price.to_double());
}

TEST(MarketMakerStrategy, QuoteSizeTapersAsInventoryApproachesLimit) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(1));
    MicrostructureCalculator micro;
    StrategyConfig cfg;
    cfg.quote_size = 10;
    cfg.max_inventory_for_skew = 100;
    MarketMakerStrategy strategy(cfg);

    auto near_limit_long = strategy.decide(book, micro, 100, Timestamp(1));
    // At the long inventory limit, the bid (which would extend the long
    // position further) should taper to zero size / stop quoting.
    EXPECT_EQ(near_limit_long.bid_size, 0);
    EXPECT_FALSE(near_limit_long.quote_bid);
    EXPECT_GT(near_limit_long.ask_size, 0);
}

TEST(MarketMakerStrategy, StopsQuotingOnStaleMarketData) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(std::chrono::seconds(1)));
    MicrostructureCalculator micro;
    StrategyConfig cfg;
    cfg.max_staleness_sec = 1.0;
    MarketMakerStrategy strategy(cfg);

    // "now" is 10 seconds after the book's last update -> stale.
    auto decision = strategy.decide(book, micro, 0, Timestamp(std::chrono::seconds(11)));
    EXPECT_FALSE(decision.quote_bid);
    EXPECT_FALSE(decision.quote_ask);
}

TEST(MarketMakerStrategy, HigherVolatilityWidensSpread) {
    auto book = make_book_with_market(99.95, 100.05, 100, Timestamp(1));
    StrategyConfig cfg;
    cfg.volatility_spread_multiplier = 2.0;
    MarketMakerStrategy strategy(cfg);

    MicrostructureCalculator calm;
    auto calm_decision = strategy.decide(book, calm, 0, Timestamp(1));

    // Feed the microstructure calculator a series of large mid-price swings
    // to build up nonzero measured short-term volatility.
    MicrostructureCalculator volatile_micro;
    double prices[] = {100.0, 101.0, 99.0, 101.5, 98.5};
    for (size_t i = 0; i < std::size(prices); ++i) {
        OrderBook b(Symbol("AAPL"));
        b.apply(MarketEvent::make_quote(Timestamp(static_cast<int64_t>(i)), i * 2 + 1, Symbol("AAPL"),
                                         Side::Buy, Price::from_double(prices[i] - 0.05), 10));
        b.apply(MarketEvent::make_quote(Timestamp(static_cast<int64_t>(i)), i * 2 + 2, Symbol("AAPL"),
                                         Side::Sell, Price::from_double(prices[i] + 0.05), 10));
        volatile_micro.on_book_update(b, Timestamp(static_cast<int64_t>(i)));
    }
    ASSERT_TRUE(volatile_micro.short_term_volatility().has_value());
    ASSERT_GT(*volatile_micro.short_term_volatility(), 0.0);

    auto volatile_decision = strategy.decide(book, volatile_micro, 0, Timestamp(1));

    double calm_spread = calm_decision.ask_price.to_double() - calm_decision.bid_price.to_double();
    double volatile_spread =
        volatile_decision.ask_price.to_double() - volatile_decision.bid_price.to_double();
    EXPECT_GT(volatile_spread, calm_spread);
}
