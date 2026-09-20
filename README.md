# Low-Latency Equity Order Book & Market-Making Engine

A C++20 market-data ingestion, order-book, market-microstructure and paper market-making system.
**Research / paper trading only: it never sends an order to a broker.**

See [docs/architecture.md](docs/architecture.md) for the pipeline diagram and thread-ownership table.

```
Finnhub WS (live trades) --> parse once --> SPSC ring buffer --+
                                                                +--> Pipeline: OrderBook -> Microstructure
Binary event log (.bin) --> ReplayEngine -----------------------+        -> Strategy -> Risk -> Paper execution
                                                                          -> Portfolio/P&L -> report
```

Live and replay share the same `Pipeline` (`src/common/pipeline.hpp`). The symbol is a runtime
`--symbol` flag; nothing is hardcoded to AAPL.

## What is real vs. simulated

| Component | Status |
|---|---|
| Finnhub WebSocket connection, subscription, JSON parsing | Real code. Connection verified against `wss://ws.finnhub.io` (see below). |
| Live trade prints | Real, but only when markets are open. Not yet observed: my only live test ran on a Sunday and received zero messages. |
| Top-of-book in live mode | **Synthetic overlay.** Finnhub's free WebSocket only sends trades, so a configurable synthetic spread is placed around each trade so the pipeline has a two-sided book. |
| Aggressor side on live trades | Inferred with the tick rule, not ground truth. |
| L2 depth used for replay and every benchmark | **Synthetic** (`src/tools/synthetic_data_gen.cpp`, seeded model). Not real market data. |
| Fills, positions, P&L | Simulated. P&L from synthetic data means nothing about real profitability. |

Live check, run Sun 2026-09-20 01:12 EDT (market closed):
`finnhub_recorder --symbol AAPL --duration-sec 15` connected and subscribed (`connected=1`),
`messages=0 malformed=0 reconnects=0`, then exited with "no events recorded". This shows the TLS
handshake and subscription work; it does **not** show trade parsing working against live data.

## Design notes

**Order book** (`src/orderbook/order_book.hpp`): per-side sorted flat `std::vector` of levels, integer
tick prices. `Quote` upserts a level (qty 0 removes), `BookDelete` removes, `Trade` updates last-trade
state only, `Snapshot` clears. Chosen by measurement (below): `std::map` was slightly faster to mutate,
the flat vector about 2x faster to read, and this pipeline is read-dominated.

**Ring buffer** (`src/concurrency/spsc_ring_buffer.hpp`): bounded SPSC; producer publishes its index with
release, consumer reads with acquire (and vice versa); head, tail and each side's cached copy of the other
index sit on separate cache lines; `push` returns false when full (never overwrites; caller counts drops).
Memory-ordering rationale is in the header comment. Lock-free is used so the I/O thread never waits on a
lock held by the consumer; the cost is fixed capacity, drop-on-overflow and strict one-producer/one-consumer use.

**Object pool** (`src/common/object_pool.hpp`): fixed-capacity free list, single-threaded. It is
benchmarked but **not currently used by the pipeline** (events are trivially copyable values).

**Microstructure** (`src/metrics/microstructure.hpp`): mid = (bid+ask)/2; spread_bps = spread/mid*1e4;
microprice = (bid*ask_qty + ask*bid_qty)/(bid_qty+ask_qty); imbalance = (bid_vol-ask_vol)/(bid_vol+ask_vol),
top-of-book and over N levels; volatility = stddev of log mid returns over a rolling window (not annualized);
rolling trade volume and trades/sec.

**Strategy** (`src/strategy/market_maker.hpp`): quotes around mid with half-spread widened by short-term
volatility; both quotes shift by `inventory_skew_bps_per_unit * clamp(inventory) * mid` (long inventory
lowers bid and ask); the inventory-extending side's size tapers to zero at the inventory limit; stops
quoting on a one-sided book or stale data. Order-book imbalance is computed but **not currently used by
the strategy**. It is a research toy, not an alpha model.

**Execution simulator** (`src/execution/paper_execution_engine.hpp`): market orders sweep opposite depth;
limit orders rest and fill when a later `Trade` prints through their price, at the limit price, with
configurable partial-fill probability, capped by the trade's size. No queue-position modelling. Seeded
RNG, so runs are reproducible.

**Risk** (`src/risk/risk_engine.hpp`): stateless pre-trade checks for order size, resulting position,
notional, daily loss, open orders and stale data. (`max_daily_loss` uses cumulative session P&L.)

**Replay** (`src/replay/`): 50-byte fixed binary records with a magic header. Events are processed in file
order using event timestamps only, and the RNG is seeded, so results are identical across runs; `--speed`
only changes pacing. Tests confirm identical final state across two runs and across speeds.

## Benchmarks

All numbers below were produced by running the binaries in this repo on **Apple M4, macOS 26.3.1,
Apple clang 17, Release build**. Numbers vary run to run; where I ran more than once I say so. macOS has
no `perf`/`heaptrack`, and Google Benchmark could not read the CPU frequency or set thread affinity here,
so treat these as indicative, not lab-grade.

**Order book** (`bench_order_book`; ~40-level bounded book, 10,000 quote events per iteration; two runs)

| Benchmark | Run 1 | Run 2 |
|---|---|---|
| Flat vector, apply 10k events | 50,826 ns | 45,490 ns |
| `std::map`, apply 10k events | 47,231 ns | 43,292 ns |
| Flat vector, best bid+ask read | 0.801 ns | 0.734 ns |
| `std::map`, best bid+ask read | 1.62 ns | 1.48 ns |
| Flat vector, top-10 depth read | 20.4 ns | 17.7 ns |
| `std::map`, top-10 depth read | 39.8 ns | 37.0 ns |

`std::map` was ~5-7% faster to apply; the flat vector ~2x faster to read. (An earlier version of my
synthetic stream let the mid price drift sub-tick every event, creating a new price level almost every
time and making `std::map` look ~2.4x faster at applies. That was a flaw in the benchmark input, fixed
by putting prices on a fixed tick grid.)

**Ring buffer** (`bench_ring_buffer`): push+pop single-threaded 0.774 ns/op; two-thread SPSC transfer of
1,048,576 items in 15.9 ms = 65.8M items/sec.

**Object pool vs new/delete** (`bench_memory`; microbenchmark of one MarketEvent-sized object in a tight loop)

| | new/delete | pool |
|---|---|---|
| acquire+release | 17.0 ns | 1.84 ns |
| 256-object burst | 4,229 ns | 424 ns |

This isolates allocator cost; it says nothing about whole-pipeline speedup, and I did **not** measure
allocation counts.

**Pipeline** (`bench_pipeline`; synthetic events): 100,000 events in 7.39 ms = 13.5M events/sec.
Decision latency (strategy + risk + order creation, measured with `steady_clock` around the requote step,
200,000-event run, 140,364 quoting decisions): p50 0.083 us, p95 0.125 us, p99 0.125 us, max 10.6 us.
The values are multiples of ~41.7 ns, consistent with the 24 MHz timer on this machine, so percentiles
are quantized. Only this one combined stage is timed; per-stage breakdown (parse, queue, book update,
etc.) is not implemented.

**Replay** (`market_engine --mode replay --speed max`): 5,000,000 synthetic events (250 MB file), three
runs: 468 ms / 499 ms / 462 ms, i.e. 10.7M / 10.0M / 10.8M events/sec (timed region is the replay loop;
file load is excluded). The run produced 8,113 quotes submitted and 2,229 fills on synthetic data.

**Tests**: 74/74 GoogleTest cases pass under `ctest`. Earlier in development the suite also passed
under ASan+UBSan (all 74) and TSan (72, excluding the slow randomized fuzz test) with no reports; I have
not re-run the sanitizers after the last benchmark/generator edits. macOS ASan cannot do leak detection.

## Limitations / not done

- No real L2 data anywhere; live-mode book is a synthetic overlay; live trade handling is untested on real trades.
- Not a queue-accurate exchange simulator.
- Fault-handling coverage is partial: sequence gaps/duplicates, empty book and stale-data quoting stop
  are tested; ring-buffer overflow, WebSocket reconnect, malformed-message and crossed-book behaviour
  have no dedicated tests (malformed/reconnect counters exist in the client).
- No per-stage latency breakdown, allocation counting, cycle-counter timing or Linux profiling.
- No MPSC queue, no multi-symbol runner, no convenience `run_tests` target (use `ctest`).
- Observability is a console end-of-run report, not structured (JSON) logs.
- README benchmark numbers are from one machine and should be regenerated on yours.

## How to run

```bash
brew install cmake googletest google-benchmark nlohmann-json   # IXWebSocket is fetched by CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd build && ctest && cd ..

./build/bin/generate_synthetic_data --symbol AAPL --events 5000000 --seed 1234 --out data/AAPL_synthetic.bin
./build/bin/market_engine --symbol AAPL --mode replay --data data/AAPL_synthetic.bin --speed max
./build/bin/bench_order_book   # also bench_ring_buffer, bench_pipeline, bench_memory

# live: put your key in config/config.local.json (git-ignored) or export FINNHUB_API_KEY
./build/bin/market_engine --symbol AAPL --mode live --duration-sec 60
./build/bin/finnhub_recorder --symbol AAPL --duration-sec 60 --out data/AAPL_live.bin
```

Sanitizers: add `-DENABLE_ASAN=ON -DENABLE_UBSAN=ON` or `-DENABLE_TSAN=ON` (use `-DBUILD_LIVE_FEED=OFF`).
