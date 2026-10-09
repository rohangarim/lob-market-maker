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
| Finnhub WebSocket connection, subscription | Real code, verified against `wss://ws.finnhub.io` (see below). |
| Finnhub message parsing and trade-to-event conversion | Real code, unit-tested against **canned** payloads in Finnhub's documented format. Never run against a live trade message. |
| Live trade prints | Real, but only when markets are open. My only live test ran on a Sunday and received zero messages. |
| Top-of-book in live mode | **Synthetic overlay.** Finnhub's free WebSocket only sends trades, so one synthetic bid and one synthetic ask are kept around the latest trade price (old overlay levels are deleted as the price moves). |
| Aggressor side on live trades | Inferred with the tick rule, not ground truth. |
| L2 depth used for replay and every pipeline benchmark | **Synthetic** (`src/tools/synthetic_book.hpp`, seeded model). Not real market data. |
| Fills, positions, P&L | Simulated. P&L from synthetic data says nothing about real profitability. |

Live check, run Sun 2026-09-20 01:12 EDT (market closed): `finnhub_recorder --symbol AAPL --duration-sec 15`
connected and subscribed (`connected=1`), `messages=0 malformed=0 reconnects=0`, then exited with "no events
recorded". This shows the TLS handshake and subscription work; it does **not** show live trade handling works.

## Correction to earlier results

The first version of this README reported 10.0-10.8M events/sec for a 5M-event replay and described a
"bounded ~40-level book". Both were wrong. The synthetic generator never deleted stale levels, so the book it
described was **crossed after 99.9991% of events** and grew past 2,600 levels; the replay measured nonsense
input. The generator was rewritten to keep a consistent book (stale levels are deleted when the reference
price moves, trades consume the levels they sweep), and a test now guards it: over 400,000 events it checks
the book stays uncrossed and under 30 levels. All pipeline/replay numbers below are from the fixed generator.

## Design notes

**Order book** (`src/orderbook/order_book.hpp`): per-side sorted flat `std::vector` of levels, integer
tick prices. `Quote` upserts a level (qty 0 removes), `BookDelete` removes, `Trade` updates last-trade
state only, `Snapshot` clears. Chosen by measurement (below): `std::map` was a few percent faster to
mutate, the flat vector about 2x faster to read, and this pipeline is read-dominated.

**Ring buffer** (`src/concurrency/spsc_ring_buffer.hpp`): bounded SPSC; producer publishes its index with
release, consumer reads with acquire (and vice versa); head, tail and each side's cached copy of the other
index sit on separate cache lines (128 bytes on Apple Silicon, whose real line size is 128 although the
standard library reports 64; `hardware_destructive_interference_size` elsewhere); `push` returns false when
full (never overwrites; the live loop counts drops). Lock-free is used so the I/O thread never waits on a
lock held by the consumer; the cost is fixed capacity, drop-on-overflow and strict one-producer/one-consumer use.

**Object pool** (`src/common/object_pool.hpp`): fixed-capacity free list, single-threaded. Benchmarked but
**not used by the pipeline** (events are trivially copyable values copied by value).

**Microstructure** (`src/metrics/microstructure.hpp`): mid = (bid+ask)/2; spread_bps = spread/mid*1e4;
microprice = (bid*ask_qty + ask*bid_qty)/(bid_qty+ask_qty); imbalance = (bid_vol-ask_vol)/(bid_vol+ask_vol),
top-of-book and over N levels; volatility = stddev of log mid returns over a rolling window (not annualized);
rolling trade volume and trades/sec.

**Strategy** (`src/strategy/market_maker.hpp`): quotes around mid with half-spread widened by short-term
volatility; both quotes shift by `inventory_skew_bps_per_unit * clamp(inventory) * mid` (long inventory
lowers bid and ask); the inventory-extending side's size tapers to zero at the inventory limit; no quotes on
a one-sided book. Order-book imbalance is computed but **not used by the strategy**. It is a research toy,
not an alpha model.

**Pipeline feed-integrity policy** (`src/common/pipeline.hpp`; all of it driven by event content and
timestamps only, so replay stays deterministic):
- sequence <= last seen (duplicate / out of order): event dropped, counted, never applied;
- sequence jumps forward (missed updates): quotes canceled, book cleared, rebuilt from later updates;
- event-time gap above `max_staleness_sec` since the previous event: same resync;
- book crossed after an event: quotes canceled, no new quotes until uncrossed;
- `on_clock(now)`: the live loop passes wall-clock time so a feed that has gone completely silent also
  cancels quotes and makes the risk engine reject new ones until the next event;
- requote hysteresis (`requote_threshold_bps`, default 1 bp): a resting quote is kept unless the new
  target differs by more than the threshold or the size changed.

**Execution simulator** (`src/execution/paper_execution_engine.hpp`): market orders sweep opposite depth;
limit orders rest and fill when a later `Trade` prints through their price, at the limit price, with
configurable partial-fill probability, capped by the trade's size. No queue-position modelling. Seeded RNG.
The strategy only places limit orders; the market-order path is tested but unused by it.

**Risk** (`src/risk/risk_engine.hpp`): stateless pre-trade checks for order size, resulting position,
notional, daily loss, open orders and stale data. (`max_daily_loss` uses cumulative session P&L.)

**Replay** (`src/replay/`): 50-byte fixed binary records with a magic header, processed in file order using
event timestamps only with a seeded RNG, so results are identical across runs on one toolchain; `--speed`
only changes pacing. Tests confirm identical final state across two runs and across speeds.

**Observability**: console end-of-run report plus `--report-json <path>` (counters, P&L, latency
percentiles). `--profile-stages` additionally times the book-update and execution stages. Latencies are kept in
a constant-memory histogram (exact below 32 ns, 16 sub-buckets per power of two above, so a reported
percentile is a bucket lower bound within ~6% of the sample).

## Benchmarks

All numbers below were produced by running the binaries in this repo on **Apple M4, macOS 26.3.1,
Apple clang 17, Release build**. Google Benchmark could not read the CPU frequency or set thread affinity here,
there is no `perf`/`heaptrack` on macOS, and the system timer ticks at 24 MHz (~41.7 ns), so treat these as
indicative, not lab-grade.

**Order book** (`bench_order_book`; 10,000 quote events per iteration on a bounded book; three runs)

| Benchmark | Run 1 | Run 2 | Run 3 |
|---|---|---|---|
| Flat vector, apply 10k events | 50,826 ns | 45,490 ns | 47,716 ns |
| `std::map`, apply 10k events | 47,231 ns | 43,292 ns | 46,124 ns |
| Flat vector, best bid+ask read | 0.801 ns | 0.734 ns | 0.756 ns |
| `std::map`, best bid+ask read | 1.62 ns | 1.48 ns | 1.51 ns |
| Flat vector, top-10 depth read | 20.4 ns | 17.7 ns | 19.7 ns |
| `std::map`, top-10 depth read | 39.8 ns | 37.0 ns | 38.4 ns |

`std::map` was 3-7% faster to apply; the flat vector about 2x faster to read.

**Ring buffer** (`bench_ring_buffer`): push+pop single-threaded 0.757 ns/op. Two-thread SPSC transfer of
1,048,576 items, four runs: 51.3 / 56.5 / 60.2 / 53.3 M items/sec. (With the old 64-byte padding two runs gave
65.8 and 58.1M; the runs overlap, so the 128-byte change shows no clear gain on this machine.)

**Object pool vs new/delete** (`bench_memory`; one MarketEvent-sized object in a tight loop)

| | new/delete | pool |
|---|---|---|
| acquire+release | 17.4 ns | 2.00 ns |
| 256-object burst | 4,152 ns | 452 ns |

This isolates allocator cost only; I did not measure allocation counts or any pipeline-level effect.

**Pipeline** (`bench_pipeline`, consistent synthetic stream): 100,000 events in 11.3 ms = 8.9M events/sec.
Stage latencies from a 200,000-event run with `profile_stages` on (all values are histogram bucket lower
bounds; a stage that finishes inside one 41.7 ns timer tick reads 0):

| Stage | samples | p50 | p95 | p99 | max |
|---|---|---|---|---|---|
| decision: strategy + risk + order (153,283 requotes) | 153k | 0.08 us | 0.124 us | 0.16 us | 27.5 us |
| book update + microstructure | 200k | 0 us | -- | 0.042 us | -- |
| execution + mark-to-market | 200k | 0 us | -- | 0.042 us | -- |

Parse, queue and market-data-receive stages are not instrumented.

**Replay** (`market_engine --mode replay --speed max`): 5,000,000 synthetic events (250 MB file, file load
excluded from timing), four runs: 8.75 / 8.79 / 8.80 / 8.51 M events/sec. That run submitted 1,018,663 orders,
canceled 1,018,491, got 243 fills (73 partial), ended at position 1 with simulated total P&L of 151.01.
Almost every order is canceled because the quote follows the touch; hysteresis cut orders from 4.98M to 1.02M
but the strategy still re-quotes on about a fifth of events, and fills are rare.
(Before hysteresis the same replay ran at 7.2-7.3M events/sec with 4.98M orders.)

**Tests**: 120/120 GoogleTest cases pass under `ctest`. On this exact code the suite also ran clean under
ASan+UBSan (all 120) and TSan (118, excluding the slow randomized fuzz test), with no reports. The new
fault-handling tests were mutation-checked: removing the duplicate-sequence guard, the crossed-book guard or
the `on_clock` cancel makes the corresponding tests fail. macOS ASan cannot do leak detection.

## Limitations / not done

- No real L2 data anywhere; live-mode book is a synthetic overlay; live trade handling is untested on real trades.
- Not a queue-accurate exchange simulator. The synthetic generator is not a calibrated market model.
- WebSocket reconnect behaviour has no automated test (the counter and re-subscribe exist). Fault tests
  cover sequence anomalies, stale feed, crossed/empty books, malformed messages (via the parser) and ring-buffer
  overflow.
- Per-stage timing covers book update, execution and decision only; no parse/queue/receive timing, no
  allocation counting, no cycle-counter timing, no Linux profiling.
- Detecting a gap only resyncs the local book; there is no snapshot request to a provider.
- Live mode polls the ring buffer with a 5 ms sleep when idle, adding up to 5 ms of queueing latency.
- No MPSC queue, no multi-symbol runner, no `run_tests` convenience target (use `ctest`).
- Benchmark numbers are from one machine and should be regenerated on yours.

## How to run

```bash
brew install cmake googletest google-benchmark nlohmann-json   # IXWebSocket is fetched by CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
cd build && ctest && cd ..

./build/bin/generate_synthetic_data --symbol AAPL --events 5000000 --seed 1234 --out data/AAPL_synthetic.bin
./build/bin/market_engine --symbol AAPL --mode replay --data data/AAPL_synthetic.bin --speed max \
    --profile-stages --report-json report.json
./build/bin/bench_order_book   # also bench_ring_buffer, bench_pipeline, bench_memory

# live: put your key in config/config.local.json (git-ignored) or export FINNHUB_API_KEY
./build/bin/market_engine --symbol AAPL --mode live --duration-sec 60
./build/bin/finnhub_recorder --symbol AAPL --duration-sec 60 --out data/AAPL_live.bin
```

Sanitizers: add `-DENABLE_ASAN=ON -DENABLE_UBSAN=ON` or `-DENABLE_TSAN=ON` (use `-DBUILD_LIVE_FEED=OFF`).
