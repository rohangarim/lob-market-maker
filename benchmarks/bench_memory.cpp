// Compares heap allocation (new/delete per event) against the fixed-
// capacity ObjectPool for MarketEvent-sized objects, to justify (or
// disprove) using a pool on the hot path. See README "Memory management"
// section for the measured numbers this benchmark actually produced.

#include <benchmark/benchmark.h>

#include "common/object_pool.hpp"
#include "marketdata/events.hpp"

using namespace lob;

static void BM_HeapAllocation_NewDeletePerEvent(benchmark::State& state) {
    for (auto _ : state) {
        auto* ev = new MarketEvent();
        ev->quantity = 100;
        benchmark::DoNotOptimize(ev);
        delete ev;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_HeapAllocation_NewDeletePerEvent);

static void BM_ObjectPool_AcquireReleasePerEvent(benchmark::State& state) {
    ObjectPool<MarketEvent> pool(4096);
    for (auto _ : state) {
        auto* ev = pool.acquire();
        ev->quantity = 100;
        benchmark::DoNotOptimize(ev);
        pool.release(ev);
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_ObjectPool_AcquireReleasePerEvent);

// Simulates a burst workload: acquire N objects before releasing any,
// closer to how a ring-buffer consumer might batch-process events.
static void BM_HeapAllocation_Burst(benchmark::State& state) {
    const int kBurst = static_cast<int>(state.range(0));
    for (auto _ : state) {
        std::vector<MarketEvent*> batch;
        batch.reserve(static_cast<size_t>(kBurst));
        for (int i = 0; i < kBurst; ++i) batch.push_back(new MarketEvent());
        for (auto* p : batch) delete p;
    }
    state.SetItemsProcessed(state.iterations() * kBurst);
}
BENCHMARK(BM_HeapAllocation_Burst)->Arg(256);

static void BM_ObjectPool_Burst(benchmark::State& state) {
    const int kBurst = static_cast<int>(state.range(0));
    ObjectPool<MarketEvent> pool(static_cast<size_t>(kBurst));
    for (auto _ : state) {
        std::vector<MarketEvent*> batch;
        batch.reserve(static_cast<size_t>(kBurst));
        for (int i = 0; i < kBurst; ++i) batch.push_back(pool.acquire());
        for (auto* p : batch) pool.release(p);
    }
    state.SetItemsProcessed(state.iterations() * kBurst);
}
BENCHMARK(BM_ObjectPool_Burst)->Arg(256);

BENCHMARK_MAIN();
