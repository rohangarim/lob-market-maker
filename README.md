# Low-Latency Equity Order Book & Market-Making Engine

A C++20 market-data, order-book, market-microstructure and **paper** market-making research system:
incremental L2 book, lock-free ingestion, a queue-position execution simulator, deterministic replay,
parallel parameter sweeps, adverse-selection analytics, and a live Finnhub feed path.

**It never sends an order to a broker.** Everything after market-data ingestion is simulated.

- [What is real and what is simulated](#what-is-real-and-what-is-simulated)
- [Architecture](#architecture)
- [Components](#components)
- [Experiments and findings](#experiments-and-findings)
- [Performance](#performance)
- [Testing](#testing)
- [Limitations](#limitations)
- [How to run](#how-to-run)
- [Corrections log](#corrections-log)

## What is real and what is simulated

| Part | Status |
|---|---|
| Order book, strategy, risk, execution simulator, portfolio, replay, sweep, live runner | Real, fully implemented code. |
| Finnhub WebSocket connection and subscription | Real; verified against `wss://ws.finnhub.io` (below). |
| Finnhub message parsing and trade-to-event conversion | Real code, unit-tested on **canned** payloads in Finnhub's documented format. **Never run against a live trade message.** |
| Live trade prints | Real when markets are open. My only live test ran on a Sunday and got zero messages. |
| Live top-of-book | **Synthetic overlay**: Finnhub's free tier streams trades only, so one synthetic bid and ask are kept around the last trade price. |
| Aggressor side on live trades | Inferred (tick rule), not ground truth. |
| All L2 depth used for replay, sweeps and benchmarks | **Synthetic** (`src/tools/synthetic_book.hpp`, seeded). Not real market data. |
| Fills, positions, P&L | Simulated. **P&L on synthetic data says nothing about real profitability** (see the experiments for why). |

Live check, Sun 2026-09-20 01:12 EDT (market closed): `finnhub_recorder --symbol AAPL --duration-sec 15`
connected and subscribed (`connected=1 messages=0 malformed=0 reconnects=0`) and recorded nothing. That proves
TLS and subscription work; it does not prove live trade handling works.

## Architecture

```
 LIVE                                               REPLAY / SWEEP
 Finnhub WS (trades)                                data/*.bin  (50-byte records)
   | WebSocket thread                                  |
   v                                                   v
 parse_finnhub_message -> TradeTapeAdapter        ReplayEngine  (event-time pacing, or max speed)
   |   (real trade + synthetic 1-level overlay)        |      N pipelines in parallel for sweeps
   v                                                   |
 SPSC ring buffer<QueuedEvent>  (drop+count if full)   |
   | consumer thread: LiveRunner                       |
   |   - stamps queue latency, records to .bin         |
   |   - on_clock(wall time), JSON log records ------> StructuredLogger thread
   +---------------------> Pipeline::process <---------+
                               |
        feed integrity: drop dup/out-of-order, resync on gaps / stale, suppress when crossed
                               |
        OrderBook (flat sorted levels) -> Microstructure -> MarketMakerStrategy
                               |
        RiskEngine -> PaperExecutionEngine (queue position, latency, taker fills, fees)
                               |
        Portfolio (P&L, fees) + MarkoutTracker + RunAnalytics -> console / JSON report
```

Threads in live mode: (1) the IXWebSocket thread (producer), (2) the consumer thread that owns the whole
`Pipeline`, recorder and stats, (3) the logger thread. `Pipeline` and everything in it is single-owner with no
locks; the only shared state is the ring buffer, an atomic drop counter and the logger's ring. Replay is
single-threaded; sweeps run one independent `Pipeline` per worker thread over a shared immutable event vector.

## Components

**Order book** (`orderbook/order_book.hpp`). Per-side sorted flat `std::vector<BookLevel>`, integer-tick prices
(`Price` is an `int64` in 1e-4 dollars). `Quote` upserts a level (qty 0 removes), `BookDelete` removes, `Trade` only
updates last-trade state, `Snapshot`/`clear()` empties. Depth reads (`bid_depth(n)`, `bids()`) return zero-allocation
spans; `quantity_at(side, price)` finds a level. Chosen over `std::map` by measurement (see Performance).

**Lock-free ring** (`concurrency/spsc_ring_buffer.hpp`). Bounded SPSC; release/acquire on each side's index, each
side keeps a private cached copy of the other's index, and the four hot fields sit on separate cache lines
(128 bytes on Apple Silicon, whose real line size is 128 though the standard library reports 64). `push` returns false
when full and never overwrites. Lock-free so the I/O thread never waits on a lock the consumer holds; the costs are
fixed capacity, drop-on-overflow and strict one-producer/one-consumer use. TSan-clean.

**Microstructure** (`metrics/microstructure.hpp`). mid; spread and spread_bps; microprice
`(bid*ask_qty + ask*bid_qty)/(bid_qty+ask_qty)`; imbalance `(bid_vol-ask_vol)/(bid_vol+ask_vol)` at the top and over N
levels; sample stddev of log mid returns over a rolling window in **O(1)** per update (running sums, rebuilt exactly
every 4096 pushes to bound drift; tested against a direct computation); rolling trade volume and trade intensity.

**Strategy** (`strategy/market_maker.hpp`). Quotes around a reference (mid, or microprice with `use_microprice`),
optionally shifted by `imbalance_skew_bps * OBI(top N levels)`; half-spread widened by short-term volatility; inventory
skew `inventory_skew_bps_per_unit * clamp(inventory) * mid` moves both quotes (long inventory lowers them); the side that
would extend a position tapers to zero size at the inventory limit. Quotes are **rounded away from the mid onto the tick
grid** and, with `post_only` (default), clamped so they can never cross the touch. Order-book imbalance and microprice
are off by default. A requote hysteresis (`requote_threshold_bps`) avoids replacing a quote for sub-threshold moves.

**Execution simulator** (`execution/paper_execution_engine.hpp`). Default **queue-position model**: an order joins the
back of the queue at its price behind the book's displayed quantity there; a trade at the price consumes the queue first
and only the excess fills it; a print strictly through the price fills the remainder; level-size drops not explained by
trades are treated as cancellations spread uniformly over the queue (the shares ahead shrink pro rata); increases join
behind. Also: **event-time order and cancel latency** (an order cannot fill before submit+latency; a cancel takes effect
only at request+latency, so fills can occur while it is in flight), **marketable limits take liquidity first** as a taker
at displayed prices, **maker/taker per-share fees** (negative = rebate), and the earlier randomized **trade-through**
model as a selectable optimistic baseline. Not modelled: hidden/iceberg liquidity, our own market impact on the
displayed book, matching-engine specifics. Prints are matched by price only (the aggressor side is ignored because
inferred sides are unreliable). Deterministic: the queue model uses no randomness.

**Pipeline feed-integrity policy** (`common/pipeline.hpp`), all driven by event content/timestamps so replay stays
reproducible: duplicate/out-of-order sequence -> dropped, counted, never applied; sequence jump or event-time gap above the
staleness threshold -> quotes canceled, book cleared, volatility history reset; crossed book -> quotes canceled and none
placed until it uncrosses; `on_clock(now)` lets a live loop cancel quotes when the feed goes silent.

**Analytics.** `MarkoutTracker` records, per fill, the edge versus the mid at the fill and the volume-weighted markout at
event-time horizons (default 1 ms, 10 ms, 100 ms, 1 s): `edge - markout` is adverse selection. `RunAnalytics` tracks max
drawdown and inventory. Reports: console and `--report-json`.

**Replay and sweeps.** `ReplayEngine` replays in file order using event timestamps only; `--speed` changes pacing, never
decisions; `--repeat N` reports min/median/max throughput. `param_sweep` replays one log under a Cartesian grid of
settings across worker threads and writes a CSV; **results are identical for any thread count** (tested).

**Live path** (`live/`). `LiveRunner` drains the ring, measures **queue latency** (enqueue to dequeue), optionally records
every received event to a replayable `.bin` (`--record`), and emits JSON-lines logs from a separate thread
(`--log-json`; stats snapshots plus anomaly records for drops, gaps, stale resets, crossed books, silent feed).
A recorded live session replays to **exactly** the same state (tested with a producer thread and more events than the ring
holds).

## Experiments and findings

All on **synthetic** data (2M events, seed 1234). The generator (`synthetic_book.hpp`) keeps a consistent bounded book on a
tick grid and can optionally add a planted imbalance signal and informed order flow. Results are a property of this
simulator, not of markets. Settings unless stated: half-spread 1.0 bps (about one tick at $190), inventory skew 0.01 bps/share,
daily-loss kill switch off, queue fill model. Markout columns are per share.

**1. The fill model matters in both directions.** Plain data:

| fill model | fills | shares | total P&L | markout @100 ms |
|---|---|---|---|---|
| queue position | 189,984 | 1,771,191 | 60,846 | 0.0344 |
| trade-through (optimistic) | 210,802 | 1,606,727 | 55,896 | 0.0347 |

The trade-through baseline gets 11.0% more fills but 9.3% fewer shares (random partials). With the default, wider quotes
(half-spread 5 bps, skew 0.5 bps/share) the queue model gets 99 fills / 979 shares against 137 fills / 1,186 shares (28% and
17% fewer). So there is no single "queue is more conservative" factor; it depends on how close to the touch the quotes sit.

**2. Uninformed flow makes market making look like free money.** With plain flow every fill has positive markout at every
horizon (P&L 60,846, max drawdown 6) because trades carry no information. With `--informed-flow 0.8` (a trade biases the
next price move) the same strategy's edge is unchanged (0.0353) but markout at 1 s falls from 0.0343 to 0.0320, so adverse
selection (edge minus markout) rises from 0.0009 to 0.0033 per share, about 3.7x. Treat any P&L from the plain generator as
an artifact; markout is the metric to read.

**3. Latency (informed flow, queue model):**

| cancel latency | fills | shares | total P&L | edge | markout @1 s |
|---|---|---|---|---|---|
| 0 | 189,096 | 1,763,578 | 56,405 | 0.0353 | 0.0320 |
| 500 us | 195,760 | 1,822,028 | 58,023 | 0.0351 | 0.0318 |
| 2 ms | 231,634 | 2,135,132 | 66,869 | 0.0343 | 0.0312 |
| 10 ms | 368,546 | 3,340,930 | 101,501 | 0.0339 | 0.0301 |

| order latency | fills | total P&L | markout @1 s |
|---|---|---|---|
| 0 | 189,096 | 56,405 | 0.0320 |
| 500 us | 180,553 | 53,901 | 0.0320 |
| 2 ms | 139,509 | 41,610 | 0.0320 |
| 10 ms | 46,838 | 13,643 | 0.0311 |

Order latency removes fills (-75% at 10 ms). Slower cancels *increase* total P&L here (stale quotes keep collecting fills)
while per-share quality degrades monotonically (markout 0.0320 to 0.0301): in this simulator adverse selection is small
next to the captured edge, so total P&L misleads and per-share markout is the right read. Real markets are harsher.

**4. Fees.** A -0.002/share maker rebate on 1,763,578 shares moved P&L by +3,527.16, exactly rebate x shares.

**5. Imbalance skew did not help, and the reason is instructive.** Sweeping `imbalance_skew_bps` over {0, 0.25, 0.5, 1, 2,
4} (about one tick = 0.5 bps) lowered P&L and markout monotonically, even on data with a planted imbalance signal. Measuring the
feature directly: the top-3 imbalance correlates **-0.22** with the mid change 10 ms later even with **no** planted
signal (-0.14 with it): the generator's mid is a noisy estimate of its hidden reference price, so bid-heavy books mean-revert,
which swamps the planted effect. So this simulator **cannot validate** imbalance skew; unit tests verify the mechanics only.
It stays off by default.

**6. A bug in my own first sweep.** With default inventory skew and tight quotes the skew pushed quotes across the spread
and the strategy paid the spread until the daily-loss limit tripped (about 2.9M risk rejects over 2M events, since each event can reject both sides). That led to
the tick grid and `post_only` (a test now asserts a long aggressive-skew replay produces zero taker fills).

## Performance

Apple M4, macOS 26.3.1, Apple clang 17, Release. Google Benchmark can't read CPU frequency or set affinity here; the
timer ticks at 24 MHz (about 41.7 ns), so small latencies are quantized. Machine noise was checked with a calibration
benchmark (single-thread ring push/pop: 0.757 ns quiet; it read 1.30 ns when the machine was busy, and I did not publish
timings from that period).

**Profile-driven optimization.** macOS `sample` on the replay loop showed the largest single entry was
`mach_continuous_time` (1,774 of 4,296 samples); with the other clock calls (`clock_gettime`, `mach_timebase_info`,
`mach_absolute_time`, stubs) **1,995 samples, 46.4% of replay time, were spent reading the clock** to time every requote. Also about 12% in the strategy recomputing rolling volatility over a 50-element `deque` on every call.
Fixes: decision latency sampled every 16th requote (`latency_sample_interval`; a fixed stride, so the percentiles are an
unbiased sample), and O(1) running-sum volatility. Interleaved A/B, tight-quote config, 3 rounds of 5 runs:
baseline median 7.13 / 7.13 / 7.11 M events/s -> **15.19 / 15.07 / 15.13**. Trading outcomes (orders, fills, position, P&L)
were bit-identical before and after on two configs. The follow-up profile is flat (largest entry `Pipeline::process` at
20.9%, clock reads 2.3%), so I stopped.

**Replay** (`market_engine --mode replay --speed max --repeat 10`, 2,000,000 synthetic events, file load excluded):

| config | min | median | max (M events/s) |
|---|---|---|---|
| default (quotes ~10 ticks out, 99 fills) | 17.86 | 18.38 | 18.51 |
| tight (half-spread 1.0 bps, 189,984 fills) | 14.76 | 15.08 | 15.27 |

`bench_pipeline`: 100,000 events in 5.42 ms = 18.5M events/s.

**Per-stage latency** (`--profile-stages`, tight config, 2M events; histogram bucket lower bounds): decision
(strategy+risk+order, sampled, n=95,778) p50 0.04 / p95 0.084 / p99 0.124 us, max 11.7 us; book update + microstructure
(n=2M) p50 0.042 / p99 0.08 us; execution + mark-to-market p50 0 / p99 0.084 us, max 177.7 us. Each stage figure includes
the cost of two clock reads, so these are upper bounds near the timer's resolution. Parse and market-data-receive are not
instrumented; queue latency is measured in live mode only.

**Order book representation** (`bench_order_book`, bounded ~40-level book, 10,000 updates per iteration, 3 runs):

| operation | flat vector | `std::map` |
|---|---|---|
| apply 10k updates | 47.2 / 47.5 / 46.6 us | 45.8 / 45.9 / 47.1 us |
| best bid + ask read | 0.761 / 0.761 / 0.764 ns | 1.56 / 1.54 / 1.59 ns |
| sum top-10 levels (allocation-free both) | 2.27 / 2.29 / 3.16 ns | 15.6 / 16.1 / 23.8 ns |

Applies are a wash (within 1-3%); reads are about 2x (top of book) and about 7x (depth walk) faster on the flat vector, which
is why it is the production representation for this read-heavy pipeline. (An earlier "depth read" benchmark compared a
span against a vector build; that was not like-for-like and was replaced.)

**Ring buffer** (`bench_ring_buffer`): single-thread push+pop 0.75 ns; two-thread transfer of 1,048,576 items 55.9 / 46.8 /
55.6 M items/s (noisy; earlier runs 51-60).

**Object pool vs `new`/`delete`** (microbenchmark, one event-sized object): 1.92 ns vs 18.3 ns; 256-object burst 450 ns vs
4,670 ns. The pipeline does not use the pool (events are values), and I did not measure allocation counts.

## Testing

207 GoogleTest cases, all passing under `ctest` (~5 s). On this exact code: ASan+UBSan 207/207; TSan 196 (it skips 11 slow
single-threaded generator-heavy tests; every threaded test, i.e. ring buffer, sweep, live runner, logger, is included). macOS
ASan has no leak detection. Coverage includes: book invariants (randomized), queue model, latency, taker fills, fees, feed-integrity
policy, parser, trade-tape adapter, markout/analytics, strategy/tick/post-only, generator consistency (uncrossed, bounded),
sweep thread-count independence, live-record-replay equivalence, JSON logger, and a fill-invariant audit (every maker fill is
triggered by a print at or through its price, on the tick grid, never as taker). Fault-handling mutations were checked: removing
the duplicate guard, crossed-book guard, `on_clock` cancel, post-only clamp, or any of four queue-model rules makes tests fail.

## Limitations

- No real L2 data anywhere. Live-mode book is a synthetic overlay; live trade handling is untested on real trades.
- The execution model is a model: no hidden liquidity, no market impact, no matching-engine specifics; prints are matched by
  price only. The synthetic generator is not calibrated and its mid is noisy and mean-reverting (finding 5); flow is uninformed
  unless `--informed-flow` is set. Do not read P&L from it as evidence of profitability.
- WebSocket reconnect has no automated test (the counter and re-subscribe exist).
- Gap detection only resyncs the local book; there is no snapshot request to a provider.
- Timing: no parse or receive-stage latency; no cycle-counter timing; no `perf`/`heaptrack` (macOS); no allocation counting.
- Single symbol per process; no MPSC queue; no `run_tests` convenience target (use `ctest`).
- Numbers are from one machine and should be regenerated on yours.

## How to run

```bash
brew install cmake googletest google-benchmark nlohmann-json     # IXWebSocket is fetched by CMake
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j
(cd build && ctest)

# data
./build/bin/generate_synthetic_data --events 2000000 --seed 1234 --out data/AAPL_plain.bin
./build/bin/generate_synthetic_data --events 2000000 --seed 1234 --informed-flow 0.8 --out data/AAPL_informed.bin

# replay (add --profile-stages, --report-json r.json, --latency-sample N, --repeat N)
./build/bin/market_engine --mode replay --data data/AAPL_plain.bin --speed max --repeat 10

# parameter sweep, parallel, deterministic; writes CSV
./build/bin/param_sweep --data data/AAPL_informed.bin --threads 8 --max-daily-loss 1e9 \
    --half-spread-bps 1.0 --cancel-latency-us 0,500,2000,10000 --fill-model queue,trade_through --out sweep.csv

# live (needs your own Finnhub key in config/config.local.json or $FINNHUB_API_KEY)
./build/bin/market_engine --mode live --duration-sec 60 --record data/AAPL_live.bin --log-json live.jsonl
./build/bin/market_engine --mode replay --data data/AAPL_live.bin       # replays that session exactly

# benchmarks and sanitizers
./build/bin/bench_pipeline ; ./build/bin/bench_order_book ; ./build/bin/bench_ring_buffer ; ./build/bin/bench_memory
cmake -S . -B build-tsan -DCMAKE_BUILD_TYPE=Debug -DENABLE_TSAN=ON -DBUILD_LIVE_FEED=OFF
```

Config lives in `config/config.example.json` (`risk`, `strategy`, `execution` sections; documented in the code).

## Corrections log

Mistakes I made and fixed, kept visible on purpose:

1. An early README reported 10.0-10.8M events/s and a "bounded ~40-level book". The generator never deleted stale levels, so the
   book was crossed after 99.9991% of events and grew past 2,600 levels. Fixed; a test guards it.
2. An early "depth read" benchmark showing 20 ns -> 0.25 ns compared a span with a vector build. Replaced with a like-for-like
   walk (finding in Performance).
3. The first strategy sweep measured how fast configs hit the daily-loss limit, not strategy quality (finding 6).
4. A first draft claimed numbers I had not yet measured; those were removed before anything was saved.
5. The clock-read overhead in the pipeline (46.4% of replay time) was invisible until it was profiled.
