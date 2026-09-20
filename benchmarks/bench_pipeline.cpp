// End-to-end pipeline throughput: order book -> microstructure -> strategy
// -> risk -> paper execution -> portfolio, driven by a synthetic L2 event
// stream, matching exactly what replay mode runs at --speed max.

#include <benchmark/benchmark.h>

#include <random>

#include "common/pipeline.hpp"

using namespace lob;

namespace {

// Reference price lives on the fixed tick grid and moves by at most one
// tick on a small fraction of steps (see synthetic_data_gen.cpp for the
// full rationale) so the book reaches a bounded, realistic working set
// instead of manufacturing a never-repeated price on almost every event.
std::vector<MarketEvent> make_synthetic_stream(size_t n) {
    std::mt19937_64 rng(9001);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<Quantity> qty_dist(1, 500);
    constexpr int64_t kTickTicks = 100;  // $0.01
    int64_t mid_ticks = 190 * Price::kScale;
    std::vector<MarketEvent> events;
    events.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (unit(rng) < 0.10) {
            mid_ticks += (unit(rng) < 0.5 ? -1 : 1) * kTickTicks;
        }
        double roll = unit(rng);
        Timestamp t{std::chrono::microseconds(static_cast<int64_t>(i) * 100)};
        SequenceNumber seq = static_cast<SequenceNumber>(i + 1);
        if (roll < 0.3) {
            Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            int64_t px_ticks = side == Side::Buy ? mid_ticks - kTickTicks / 2 : mid_ticks + kTickTicks / 2;
            events.push_back(MarketEvent::make_trade(t, seq, Symbol("AAPL"), side,
                                                       Price::from_ticks(px_ticks), qty_dist(rng)));
        } else {
            Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            int level = static_cast<int>(std::pow(unit(rng), 2.0) * 10);
            int64_t px_ticks = side == Side::Buy ? mid_ticks - kTickTicks * (level + 1)
                                                  : mid_ticks + kTickTicks * (level + 1);
            events.push_back(MarketEvent::make_quote(t, seq, Symbol("AAPL"), side,
                                                       Price::from_ticks(px_ticks), qty_dist(rng)));
        }
    }
    return events;
}

}  // namespace

static void BM_Pipeline_EndToEndThroughput(benchmark::State& state) {
    auto events = make_synthetic_stream(static_cast<size_t>(state.range(0)));
    for (auto _ : state) {
        PipelineConfig cfg;
        Pipeline pipeline(Symbol("AAPL"), cfg);
        for (const auto& ev : events) pipeline.process(ev);
        Quantity final_position = pipeline.portfolio().state().position;
        benchmark::DoNotOptimize(final_position);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * events.size()));
}
BENCHMARK(BM_Pipeline_EndToEndThroughput)->Arg(100'000)->Unit(benchmark::kMillisecond);

// Reports decision-latency percentiles (signal calc -> strategy -> risk ->
// order creation) as benchmark counters rather than throughput, since that
// is the number this project's latency budget actually cares about.
static void BM_Pipeline_DecisionLatencyPercentiles(benchmark::State& state) {
    auto events = make_synthetic_stream(static_cast<size_t>(state.range(0)));
    PipelineConfig cfg;
    Pipeline pipeline(Symbol("AAPL"), cfg);
    for (auto _ : state) {
        for (const auto& ev : events) pipeline.process(ev);
    }
    const auto& lat = pipeline.stats().decision_latency;
    state.counters["p50_us"] = lat.percentile(50) / 1000.0;
    state.counters["p95_us"] = lat.percentile(95) / 1000.0;
    state.counters["p99_us"] = lat.percentile(99) / 1000.0;
    state.counters["max_us"] = lat.max_ns() / 1000.0;
    state.counters["samples"] = static_cast<double>(lat.count());
}
BENCHMARK(BM_Pipeline_DecisionLatencyPercentiles)->Arg(200'000)->Iterations(1);

BENCHMARK_MAIN();
