#include <gtest/gtest.h>

#include <random>

#include "replay/replay_engine.hpp"

using namespace lob;

namespace {

std::vector<MarketEvent> generate_events(uint64_t seed, int count) {
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> walk(-0.02, 0.02);
    std::uniform_int_distribution<Quantity> qty_dist(1, 100);
    std::uniform_int_distribution<int> kind_dist(0, 9);

    std::vector<MarketEvent> events;
    double mid = 100.0;
    SequenceNumber seq = 0;
    for (int i = 0; i < count; ++i) {
        mid = std::max(1.0, mid + walk(rng));
        ++seq;
        Timestamp t{std::chrono::milliseconds(i)};
        int kind = kind_dist(rng);
        if (kind < 6) {
            Side side = (kind % 2 == 0) ? Side::Buy : Side::Sell;
            double px = side == Side::Buy ? mid - 0.05 : mid + 0.05;
            events.push_back(
                MarketEvent::make_quote(t, seq, Symbol("AAPL"), side, Price::from_double(px), qty_dist(rng)));
        } else {
            Side side = (kind % 2 == 0) ? Side::Buy : Side::Sell;
            events.push_back(MarketEvent::make_trade(t, seq, Symbol("AAPL"), side,
                                                       Price::from_double(mid), qty_dist(rng)));
        }
    }
    return events;
}

struct RunResult {
    Quantity position;
    double realized_pnl;
    double total_pnl;
    size_t fills;
    size_t quotes_submitted;
    size_t events_processed;
};

RunResult run_once(const std::vector<MarketEvent>& events) {
    PipelineConfig cfg;
    cfg.fill_assumptions.rng_seed = 555;
    ReplayEngine engine(Symbol("AAPL"), cfg);
    engine.run(events, /*speed=*/0.0);  // max speed, no pacing delay
    const auto& p = engine.pipeline().portfolio().state();
    const auto& s = engine.pipeline().stats();
    return RunResult{p.position, p.realized_pnl, p.total_pnl, s.fills, s.quotes_submitted,
                      s.events_processed};
}

}  // namespace

TEST(ReplayDeterminism, IdenticalInputProducesIdenticalFinalState) {
    auto events = generate_events(42, 20'000);
    RunResult a = run_once(events);
    RunResult b = run_once(events);

    EXPECT_EQ(a.position, b.position);
    EXPECT_DOUBLE_EQ(a.realized_pnl, b.realized_pnl);
    EXPECT_DOUBLE_EQ(a.total_pnl, b.total_pnl);
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_EQ(a.quotes_submitted, b.quotes_submitted);
    EXPECT_EQ(a.events_processed, b.events_processed);
}

TEST(ReplayDeterminism, ResultIndependentOfPlaybackSpeedSetting) {
    // Speed only paces wall-clock sleeping between events; it must not
    // change the sequence of decisions made. Use a small event count since
    // this test actually sleeps at speed=1.
    auto events = generate_events(7, 300);

    PipelineConfig cfg;
    cfg.fill_assumptions.rng_seed = 111;
    ReplayEngine max_speed(Symbol("AAPL"), cfg);
    max_speed.run(events, 0.0);

    ReplayEngine paced(Symbol("AAPL"), cfg);
    paced.run(events, 1000.0);  // fast but nonzero pacing

    EXPECT_EQ(max_speed.pipeline().portfolio().state().position,
              paced.pipeline().portfolio().state().position);
    EXPECT_EQ(max_speed.pipeline().stats().fills, paced.pipeline().stats().fills);
}
