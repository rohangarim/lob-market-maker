# Architecture

See the diagram and component descriptions in the top-level README. This file records ownership and the
threading model precisely.

```
 LIVE                                               REPLAY / SWEEP
 Finnhub WS (trades)                                data/*.bin
   | WebSocket thread (producer)                       |
   v                                                   v
 parse_finnhub_message -> TradeTapeAdapter        ReplayEngine / run_sweep
   v                                                   |
 SPSC ring<QueuedEvent>                                |
   | consumer thread: LiveRunner                       |
   |   recorder, queue-latency stamps, on_clock        |
   |   StructuredLogger (own thread) <- log records    |
   +---------------------> Pipeline::process <---------+
                               v
   feed integrity -> OrderBook -> Microstructure -> Strategy -> Risk -> PaperExecutionEngine -> Portfolio
                               v
                  MarkoutTracker, RunAnalytics, PipelineStats -> report
```

## Threads and ownership (live mode)

| Thread | Owns | Notes |
|---|---|---|
| IXWebSocket thread | `FinnhubClient` counters (atomics), `TradeTapeAdapter` + its batch buffer | Parses once, stamps and pushes `QueuedEvent`; on a full ring it drops and bumps an atomic counter. |
| Consumer thread | `Pipeline` and all of its state, `BinaryEventRecorder`, `LiveRunnerStats` | No locks. Single owner. Calls `on_clock()` with wall time. |
| Logger thread | the output stream, the logger's own SPSC ring consumer side | The consumer thread is that ring's single producer; formatting happens only here. |

Shared state: the event ring (release/acquire indices), the drop counter (relaxed atomic), the logger ring, and the logger's
written/dropped counters. Nothing else crosses threads.

Replay is single-threaded. A sweep runs one independent `Pipeline` per point on worker threads over a shared **immutable** event
vector and writes results into a pre-sized vector by index, so output is identical for any thread count (tested, TSan-clean).

## Time

Two clocks are in play and must not be mixed up. *Event time* (feed timestamps) drives every decision that must replay
deterministically: staleness resync, order and cancel latency, markout horizons. *Wall-clock time* is used only for
measuring latency (queue and stage timers, `steady_clock`) and for `on_clock()` in live mode, whose argument must share an epoch
with event timestamps (it does: both are Unix-epoch nanoseconds).

## Feed integrity

Duplicate/out-of-order events are dropped; a sequence gap or a large event-time gap cancels quotes, clears the book and resets
volatility history; a crossed book cancels quotes and blocks new ones; a silent feed (`on_clock`) cancels quotes and makes the
risk engine reject new orders until the next event.
