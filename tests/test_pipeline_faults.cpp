#include <gtest/gtest.h>

#include "common/pipeline.hpp"

using namespace lob;

namespace {

Timestamp sec(double s) {
    return std::chrono::duration_cast<Timestamp>(std::chrono::duration<double>(s));
}
MarketEvent quote(SequenceNumber seq, Side side, double px, Quantity qty, Timestamp t) {
    return MarketEvent::make_quote(t, seq, Symbol("AAPL"), side, Price::from_double(px), qty);
}
MarketEvent del(SequenceNumber seq, Side side, double px, Timestamp t) {
    return MarketEvent::make_delete(t, seq, Symbol("AAPL"), side, Price::from_double(px));
}

PipelineConfig cfg() {
    PipelineConfig c;
    c.strategy.max_staleness_sec = 5.0;
    c.fill_assumptions.partial_fill_probability = 0.0;
    return c;
}

// A pipeline that already holds a two-sided book and two resting quotes.
Pipeline quoting_pipeline() {
    Pipeline p(Symbol("AAPL"), cfg());
    p.process(quote(1, Side::Buy, 99.95, 100, sec(1)));
    p.process(quote(2, Side::Sell, 100.05, 100, sec(1)));
    return p;
}

}  // namespace

TEST(PipelineFaults, BaselineQuotesTwoSides) {
    auto p = quoting_pipeline();
    EXPECT_EQ(p.execution().open_order_count(), 2u);
}

TEST(PipelineFaults, DuplicateSequenceIsDroppedAndNotApplied) {
    auto p = quoting_pipeline();
    p.process(quote(2, Side::Buy, 99.99, 50, sec(1)));  // duplicate of seq 2
    EXPECT_EQ(p.stats().out_of_order_dropped, 1u);
    EXPECT_EQ(p.stats().events_processed, 3u);
    EXPECT_DOUBLE_EQ(p.book().best_bid()->to_double(), 99.95);  // the duplicate was not applied
    EXPECT_EQ(p.stats().sequence_gaps, 0u);
}

TEST(PipelineFaults, OutOfOrderSequenceIsDroppedAndDoesNotRewindState) {
    auto p = quoting_pipeline();
    p.process(quote(3, Side::Buy, 99.96, 10, sec(1)));
    p.process(quote(2, Side::Buy, 99.97, 10, sec(1)));  // arrived late
    EXPECT_EQ(p.stats().out_of_order_dropped, 1u);
    EXPECT_DOUBLE_EQ(p.book().best_bid()->to_double(), 99.96);
    p.process(quote(4, Side::Buy, 99.96, 20, sec(1)));  // stream continues normally from 3
    EXPECT_EQ(p.stats().sequence_gaps, 0u);
}

TEST(PipelineFaults, SequenceGapCancelsQuotesAndClearsBook) {
    auto p = quoting_pipeline();
    ASSERT_EQ(p.execution().open_order_count(), 2u);
    p.process(quote(10, Side::Buy, 99.90, 10, sec(1)));  // seqs 3..9 missed
    EXPECT_EQ(p.stats().sequence_gaps, 1u);
    EXPECT_EQ(p.execution().open_order_count(), 0u);
    EXPECT_EQ(p.book().ask_level_count(), 0u);  // old levels discarded
    EXPECT_EQ(p.book().bid_level_count(), 1u);  // only the post-gap update remains
}

TEST(PipelineFaults, LargeEventTimeGapResyncs) {
    auto p = quoting_pipeline();
    p.process(quote(3, Side::Buy, 99.90, 10, sec(30)));  // 29 s silence, threshold 5 s
    EXPECT_EQ(p.stats().stale_resets, 1u);
    EXPECT_EQ(p.execution().open_order_count(), 0u);
    EXPECT_EQ(p.book().ask_level_count(), 0u);
    EXPECT_EQ(p.stats().sequence_gaps, 0u);
}

TEST(PipelineFaults, GapBelowThresholdDoesNotResync) {
    auto p = quoting_pipeline();
    p.process(quote(3, Side::Buy, 99.96, 10, sec(4)));  // 3 s < 5 s
    EXPECT_EQ(p.stats().stale_resets, 0u);
    EXPECT_EQ(p.book().ask_level_count(), 1u);
}

TEST(PipelineFaults, CrossedBookCancelsQuotesAndBlocksNewOnes) {
    auto p = quoting_pipeline();
    ASSERT_EQ(p.execution().open_order_count(), 2u);

    p.process(quote(3, Side::Buy, 100.10, 10, sec(1)));  // bid through the ask
    EXPECT_TRUE(p.book().crossed());
    EXPECT_EQ(p.stats().crossed_book_events, 1u);
    EXPECT_EQ(p.execution().open_order_count(), 0u);

    p.process(del(4, Side::Buy, 100.10, sec(1)));  // uncross
    EXPECT_FALSE(p.book().crossed());
    EXPECT_EQ(p.execution().open_order_count(), 2u);  // quoting resumes
}

TEST(PipelineFaults, InitiallyCrossedBookNeverQuotes) {
    Pipeline p(Symbol("AAPL"), cfg());
    p.process(quote(1, Side::Buy, 100.10, 100, sec(1)));
    p.process(quote(2, Side::Sell, 100.00, 100, sec(1)));
    EXPECT_EQ(p.stats().quotes_submitted, 0u);
    EXPECT_EQ(p.stats().crossed_book_events, 1u);
}

TEST(PipelineFaults, EmptyBookAfterResyncDoesNotQuote) {
    auto p = quoting_pipeline();
    p.process(quote(9, Side::Sell, 100.05, 10, sec(1)));  // gap -> cleared, one-sided
    EXPECT_EQ(p.execution().open_order_count(), 0u);
    EXPECT_FALSE(p.book().mid_price().has_value());
}

TEST(PipelineFaults, OnClockCancelsQuotesWhenFeedGoesSilent) {
    auto p = quoting_pipeline();
    p.on_clock(sec(3));  // within threshold
    EXPECT_EQ(p.execution().open_order_count(), 2u);
    EXPECT_FALSE(p.market_data_stale());

    p.on_clock(sec(20));
    EXPECT_TRUE(p.market_data_stale());
    EXPECT_EQ(p.stats().clock_stale_events, 1u);
    EXPECT_EQ(p.execution().open_order_count(), 0u);

    p.on_clock(sec(21));  // already stale: not double counted
    EXPECT_EQ(p.stats().clock_stale_events, 1u);
}

TEST(PipelineFaults, FreshEventClearsClockStaleFlag) {
    auto p = quoting_pipeline();
    p.on_clock(sec(20));
    ASSERT_TRUE(p.market_data_stale());
    p.process(quote(3, Side::Buy, 99.96, 10, sec(20)));
    EXPECT_FALSE(p.market_data_stale());
}

TEST(PipelineFaults, OnClockBeforeAnyEventIsNoop) {
    Pipeline p(Symbol("AAPL"), cfg());
    p.on_clock(sec(100));
    EXPECT_FALSE(p.market_data_stale());
    EXPECT_EQ(p.stats().clock_stale_events, 0u);
}

TEST(PipelineFaults, ResyncResetsVolatilityHistory) {
    auto p = quoting_pipeline();
    p.process(quote(3, Side::Buy, 99.96, 10, sec(1)));
    p.process(quote(4, Side::Buy, 99.94, 10, sec(1)));
    p.process(quote(5, Side::Buy, 99.97, 10, sec(1)));
    ASSERT_TRUE(p.microstructure().short_term_volatility().has_value());
    p.process(quote(50, Side::Buy, 99.90, 10, sec(1)));  // gap -> resync
    EXPECT_FALSE(p.microstructure().short_term_volatility().has_value());
}

TEST(PipelineFaults, StageProfilingRecordsOneSamplePerAppliedEvent) {
    PipelineConfig c = cfg();
    c.profile_stages = true;
    Pipeline p(Symbol("AAPL"), c);
    for (SequenceNumber i = 1; i <= 50; ++i) {
        p.process(quote(i, i % 2 ? Side::Buy : Side::Sell, i % 2 ? 99.95 : 100.05, 10, sec(1)));
    }
    EXPECT_EQ(p.stats().book_update_latency.count(), 50u);
    EXPECT_EQ(p.stats().execution_latency.count(), 50u);
}

TEST(PipelineHysteresis, SmallTargetWobbleKeepsRestingQuotes) {
    PipelineConfig c = cfg();
    c.strategy.requote_threshold_bps = 5.0;  // ~$0.05 at 100
    c.strategy.volatility_spread_multiplier = 0.0;
    Pipeline p(Symbol("AAPL"), c);
    p.process(quote(1, Side::Buy, 99.95, 100, sec(1)));
    p.process(quote(2, Side::Sell, 100.05, 100, sec(1)));
    ASSERT_EQ(p.stats().quotes_submitted, 2u);
    p.process(quote(3, Side::Buy, 99.96, 100, sec(1)));  // mid moves ~0.5 cent -> target within threshold
    EXPECT_EQ(p.stats().quotes_submitted, 2u);
    EXPECT_EQ(p.stats().quotes_canceled, 0u);
}

TEST(PipelineHysteresis, LargeMoveReplacesQuotes) {
    PipelineConfig c = cfg();
    c.strategy.requote_threshold_bps = 5.0;
    c.strategy.volatility_spread_multiplier = 0.0;
    Pipeline p(Symbol("AAPL"), c);
    p.process(quote(1, Side::Buy, 99.95, 100, sec(1)));
    p.process(quote(2, Side::Sell, 100.05, 100, sec(1)));
    // Move the ask up without ever crossing or going one-sided: add the new
    // level first, then remove the old one. Mid goes 100.00 -> 100.20.
    p.process(quote(3, Side::Sell, 100.45, 100, sec(1)));
    ASSERT_EQ(p.stats().quotes_submitted, 2u) << "adding a deeper level must not move the mid";
    p.process(del(4, Side::Sell, 100.05, sec(1)));
    EXPECT_GE(p.stats().quotes_canceled, 1u);
    EXPECT_GT(p.stats().quotes_submitted, 2u);
}

TEST(PipelineHysteresis, ZeroThresholdReplacesOnAnyTick) {
    PipelineConfig c = cfg();
    c.strategy.requote_threshold_bps = 0.0;
    c.strategy.volatility_spread_multiplier = 0.0;
    Pipeline p(Symbol("AAPL"), c);
    p.process(quote(1, Side::Buy, 99.95, 100, sec(1)));
    p.process(quote(2, Side::Sell, 100.05, 100, sec(1)));
    p.process(quote(3, Side::Buy, 99.96, 100, sec(1)));
    EXPECT_GT(p.stats().quotes_submitted, 2u);
}
