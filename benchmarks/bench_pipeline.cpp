// End-to-end pipeline throughput: order book -> microstructure -> strategy
// -> risk -> paper execution -> portfolio, driven by a synthetic L2 event
// stream, matching exactly what replay mode runs at --speed max.

#include <benchmark/benchmark.h>

#include <random>

#include "common/pipeline.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;

namespace {

// Consistent synthetic L2 stream (see tools/synthetic_book.hpp): bounded book,
// stale levels deleted, trades consume liquidity.
std::vector<MarketEvent> make_synthetic_stream(size_t n) {
    SyntheticBookGenerator gen(9001, 190.0, 0.01, Symbol("AAPL"));
    std::vector<MarketEvent> events;
    events.reserve(n);
    for (size_t i = 0; i < n; ++i) events.push_back(gen.next());
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
    cfg.profile_stages = true;
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
    const auto& bk = pipeline.stats().book_update_latency;
    const auto& ex = pipeline.stats().execution_latency;
    state.counters["book_p50_us"] = bk.percentile(50) / 1000.0;
    state.counters["book_p99_us"] = bk.percentile(99) / 1000.0;
    state.counters["exec_p50_us"] = ex.percentile(50) / 1000.0;
    state.counters["exec_p99_us"] = ex.percentile(99) / 1000.0;
    state.counters["crossed"] = static_cast<double>(pipeline.stats().crossed_book_events);
    state.counters["fills"] = static_cast<double>(pipeline.stats().fills);
}
BENCHMARK(BM_Pipeline_DecisionLatencyPercentiles)->Arg(200'000)->Iterations(1);

BENCHMARK_MAIN();
