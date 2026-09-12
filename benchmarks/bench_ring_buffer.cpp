// Benchmarks the SPSC ring buffer's single-threaded push/pop cost and its
// real cross-core producer/consumer throughput.

#include <benchmark/benchmark.h>

#include <thread>

#include "concurrency/spsc_ring_buffer.hpp"

using namespace lob;

static void BM_RingBuffer_PushPopSingleThreaded(benchmark::State& state) {
    SpscRingBuffer<uint64_t, 1 << 12> rb;
    uint64_t v = 0;
    for (auto _ : state) {
        rb.push(v);
        benchmark::DoNotOptimize(rb.pop());
        ++v;
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
}
BENCHMARK(BM_RingBuffer_PushPopSingleThreaded);

static void BM_RingBuffer_CrossThreadThroughput(benchmark::State& state) {
    for (auto _ : state) {
        state.PauseTiming();
        SpscRingBuffer<uint64_t, 1 << 16> rb;
        const uint64_t kCount = static_cast<uint64_t>(state.range(0));
        state.ResumeTiming();

        std::thread producer([&] {
            uint64_t i = 0;
            while (i < kCount) {
                if (rb.push(i)) ++i;
            }
        });
        uint64_t consumed = 0;
        while (consumed < kCount) {
            if (auto v = rb.pop()) {
                benchmark::DoNotOptimize(*v);
                ++consumed;
            }
        }
        producer.join();
        state.SetItemsProcessed(static_cast<int64_t>(kCount));
    }
}
BENCHMARK(BM_RingBuffer_CrossThreadThroughput)->Arg(1 << 20)->Unit(benchmark::kMillisecond);

BENCHMARK_MAIN();
