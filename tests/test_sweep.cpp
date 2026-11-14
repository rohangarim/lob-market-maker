#include <gtest/gtest.h>

#include "config/pipeline_config_json.hpp"
#include "replay/sweep.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;

namespace {

std::vector<MarketEvent> make_events(size_t n, double strength = 0.0) {
    SyntheticBookGenerator g(11, 190.0, 0.01, Symbol("AAPL"), strength);
    std::vector<MarketEvent> ev;
    ev.reserve(n);
    for (size_t i = 0; i < n; ++i) ev.push_back(g.next());
    return ev;
}

std::vector<SweepPoint> make_grid() {
    std::vector<SweepPoint> pts;
    for (double hs : {2.0, 5.0, 12.0}) {
        for (double imb : {0.0, 8.0}) {
            for (auto fm : {FillModel::QueuePosition, FillModel::TradeThrough}) {
                SweepPoint p;
                p.label = "hs=" + std::to_string(hs) + " imb=" + std::to_string(imb);
                p.config.strategy.base_half_spread_bps = hs;
                p.config.strategy.imbalance_skew_bps = imb;
                p.config.fill_assumptions.fill_model = fm;
                pts.push_back(std::move(p));
            }
        }
    }
    return pts;
}

void expect_same(const SweepResult& a, const SweepResult& b) {
    EXPECT_EQ(a.label, b.label);
    EXPECT_EQ(a.events, b.events);
    EXPECT_EQ(a.orders_submitted, b.orders_submitted);
    EXPECT_EQ(a.orders_canceled, b.orders_canceled);
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_EQ(a.taker_fills, b.taker_fills);
    EXPECT_EQ(a.final_position, b.final_position);
    EXPECT_EQ(a.max_abs_position, b.max_abs_position);
    EXPECT_DOUBLE_EQ(a.realized_pnl, b.realized_pnl);
    EXPECT_DOUBLE_EQ(a.total_pnl, b.total_pnl);
    EXPECT_DOUBLE_EQ(a.max_drawdown, b.max_drawdown);
    EXPECT_DOUBLE_EQ(a.avg_edge, b.avg_edge);
    ASSERT_EQ(a.avg_markout.size(), b.avg_markout.size());
    for (size_t i = 0; i < a.avg_markout.size(); ++i) EXPECT_DOUBLE_EQ(a.avg_markout[i], b.avg_markout[i]);
}

}  // namespace

TEST(Sweep, ResultsAreIdenticalForAnyThreadCount) {
    auto events = make_events(60'000);
    auto grid = make_grid();
    auto one = run_sweep(events, Symbol("AAPL"), grid, 1);
    auto four = run_sweep(events, Symbol("AAPL"), grid, 4);
    auto many = run_sweep(events, Symbol("AAPL"), grid, 64);  // more threads than points
    ASSERT_EQ(one.size(), grid.size());
    ASSERT_EQ(four.size(), grid.size());
    ASSERT_EQ(many.size(), grid.size());
    for (size_t i = 0; i < grid.size(); ++i) {
        SCOPED_TRACE(grid[i].label);
        expect_same(one[i], four[i]);
        expect_same(one[i], many[i]);
    }
}

TEST(Sweep, EachPointMatchesAStandaloneReplay) {
    auto events = make_events(40'000);
    auto grid = make_grid();
    auto results = run_sweep(events, Symbol("AAPL"), grid, 3);
    for (size_t i = 0; i < grid.size(); ++i) {
        ReplayEngine solo(Symbol("AAPL"), grid[i].config);
        solo.run(events, 0.0);
        EXPECT_EQ(results[i].fills, solo.pipeline().stats().fills) << grid[i].label;
        EXPECT_DOUBLE_EQ(results[i].total_pnl, solo.pipeline().portfolio().state().total_pnl) << grid[i].label;
    }
}

TEST(Sweep, DifferentSettingsActuallyProduceDifferentOutcomes) {
    auto events = make_events(80'000);
    auto results = run_sweep(events, Symbol("AAPL"), make_grid(), 2);
    bool any_diff = false;
    for (size_t i = 1; i < results.size(); ++i)
        if (results[i].fills != results[0].fills || results[i].orders_submitted != results[0].orders_submitted) any_diff = true;
    EXPECT_TRUE(any_diff) << "a sweep whose rows are all identical is not sweeping anything";
}

TEST(Sweep, EmptyGridAndEmptyLogAreHandled) {
    auto events = make_events(100);
    EXPECT_TRUE(run_sweep(events, Symbol("AAPL"), {}, 4).empty());
    SweepPoint p;
    p.label = "x";
    auto r = run_sweep({}, Symbol("AAPL"), {p}, 2);
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r[0].events, 0u);
}

TEST(Sweep, MarkoutHorizonsFlowThrough) {
    auto events = make_events(50'000);
    SweepPoint p;
    p.label = "x";
    auto r = run_sweep(events, Symbol("AAPL"), {p}, 1);
    EXPECT_EQ(r[0].avg_markout.size(), p.config.markout_horizons.size());
    EXPECT_EQ(r[0].markout_qty.size(), p.config.markout_horizons.size());
}

// ---- config parsing -------------------------------------------------------

TEST(ConfigJson, ParsesExecutionAndStrategyFields) {
    auto cfg = nlohmann::json::parse(R"({
        "strategy": {"base_half_spread_bps": 7.5, "use_microprice": true,
                     "imbalance_skew_bps": 4.0, "imbalance_levels": 5},
        "execution": {"fill_model": "trade_through", "order_latency_us": 250,
                      "cancel_latency_us": 400, "maker_fee_per_share": -0.002,
                      "taker_fee_per_share": 0.003, "rng_seed": 9},
        "risk": {"max_position": 77}
    })");
    auto c = build_pipeline_config(cfg);
    EXPECT_DOUBLE_EQ(c.strategy.base_half_spread_bps, 7.5);
    EXPECT_TRUE(c.strategy.use_microprice);
    EXPECT_DOUBLE_EQ(c.strategy.imbalance_skew_bps, 4.0);
    EXPECT_EQ(c.strategy.imbalance_levels, 5);
    EXPECT_EQ(c.fill_assumptions.fill_model, FillModel::TradeThrough);
    EXPECT_EQ(c.fill_assumptions.order_latency, std::chrono::microseconds(250));
    EXPECT_EQ(c.fill_assumptions.cancel_latency, std::chrono::microseconds(400));
    EXPECT_DOUBLE_EQ(c.fill_assumptions.maker_fee_per_share, -0.002);
    EXPECT_DOUBLE_EQ(c.fill_assumptions.taker_fee_per_share, 0.003);
    EXPECT_EQ(c.fill_assumptions.rng_seed, 9u);
    EXPECT_EQ(c.risk_limits.max_position, 77);
}

TEST(ConfigJson, MissingSectionsKeepDefaults) {
    auto c = build_pipeline_config(nlohmann::json::object());
    PipelineConfig d;
    EXPECT_EQ(c.fill_assumptions.fill_model, d.fill_assumptions.fill_model);
    EXPECT_DOUBLE_EQ(c.strategy.base_half_spread_bps, d.strategy.base_half_spread_bps);
}

TEST(ConfigJson, UnknownFillModelIsAnErrorNotASilentDefault) {
    auto cfg = nlohmann::json::parse(R"({"execution": {"fill_model": "magic"}})");
    EXPECT_THROW(build_pipeline_config(cfg), std::runtime_error);
}

TEST(ConfigJson, DefaultFillModelIsQueuePosition) {
    EXPECT_EQ(build_pipeline_config(nlohmann::json::object()).fill_assumptions.fill_model,
              FillModel::QueuePosition);
}
