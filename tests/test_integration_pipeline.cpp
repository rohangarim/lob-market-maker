#include <gtest/gtest.h>

#include "common/pipeline.hpp"

using namespace lob;

namespace {
MarketEvent quote(SequenceNumber seq, Side side, double price, Quantity qty, Timestamp ts) {
    return MarketEvent::make_quote(ts, seq, Symbol("AAPL"), side, Price::from_double(price), qty);
}
MarketEvent trade(SequenceNumber seq, Side side, double price, Quantity qty, Timestamp ts) {
    return MarketEvent::make_trade(ts, seq, Symbol("AAPL"), side, Price::from_double(price), qty);
}
}  // namespace

// End-to-end integration test: market data -> order book -> strategy ->
// risk -> paper execution -> portfolio/P&L, all through the shared
// Pipeline that both live and replay mode drive.
TEST(PipelineIntegration, FullFlowFromQuotesToFillToPnl) {
    PipelineConfig cfg;
    cfg.strategy.base_half_spread_bps = 10.0;
    cfg.strategy.quote_size = 10;
    cfg.fill_assumptions.partial_fill_probability = 0.0;  // deterministic full fills
    Pipeline pipeline(Symbol("AAPL"), cfg);

    Timestamp t(std::chrono::seconds(1));
    pipeline.process(quote(1, Side::Buy, 99.95, 500, t));
    pipeline.process(quote(2, Side::Sell, 100.05, 500, t));

    ASSERT_EQ(pipeline.stats().quotes_submitted, 2u);
    ASSERT_EQ(pipeline.execution().open_order_count(), 2u);

    auto orders = pipeline.execution().open_orders();
    ASSERT_EQ(orders.size(), 2u);
    Price our_bid = (orders[0].side == Side::Buy) ? orders[0].price : orders[1].price;

    // A sell-side trade printing at/below our resting bid should fill it.
    pipeline.process(trade(3, Side::Sell, our_bid.to_double() - 0.001, 10, t));

    EXPECT_EQ(pipeline.stats().fills, 1u);
    EXPECT_EQ(pipeline.portfolio().state().position, 10);
    EXPECT_GT(pipeline.portfolio().state().total_volume_traded, 0);
}

TEST(PipelineIntegration, RiskLimitBlocksQuoteBeyondMaxPosition) {
    PipelineConfig cfg;
    cfg.risk_limits.max_position = 5;
    cfg.strategy.quote_size = 100;  // strategy wants to quote much more than risk allows
    cfg.fill_assumptions.partial_fill_probability = 0.0;
    Pipeline pipeline(Symbol("AAPL"), cfg);

    Timestamp t(std::chrono::seconds(1));
    pipeline.process(quote(1, Side::Buy, 99.95, 500, t));
    pipeline.process(quote(2, Side::Sell, 100.05, 500, t));

    // Both sides rejected: 100-share quote size would exceed max_position of 5
    // in either direction from a flat start.
    EXPECT_EQ(pipeline.stats().risk_rejects, 2u);
    EXPECT_EQ(pipeline.execution().open_order_count(), 0u);
}

TEST(PipelineIntegration, SequenceGapIsCounted) {
    PipelineConfig cfg;
    Pipeline pipeline(Symbol("AAPL"), cfg);
    Timestamp t(std::chrono::seconds(1));
    pipeline.process(quote(1, Side::Buy, 99.95, 100, t));
    pipeline.process(quote(5, Side::Buy, 99.94, 100, t));  // jumped from 1 to 5
    EXPECT_EQ(pipeline.stats().sequence_gaps, 1u);
}

TEST(PipelineIntegration, EmptyBookProducesNoQuotesAndNoCrash) {
    PipelineConfig cfg;
    Pipeline pipeline(Symbol("AAPL"), cfg);
    Timestamp t(std::chrono::seconds(1));
    pipeline.process(MarketEvent::make_heartbeat(t, 1, Symbol("AAPL")));
    EXPECT_EQ(pipeline.stats().quotes_submitted, 0u);
    EXPECT_FALSE(pipeline.book().best_bid().has_value());
}
