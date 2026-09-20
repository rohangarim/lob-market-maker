# Architecture

```
 LIVE                                              REPLAY
 Finnhub WebSocket (trade prints)                  data/*.bin (binary event log)
        |                                                  |
        v                                                  v
 FinnhubClient (IXWebSocket callback thread)        ReplayEngine (reads file, optional pacing)
   JSON -> MarketEvent, parsed once                         |
        |                                                   |
        v                                                   |
 SpscRingBuffer<MarketEvent>  (drop + count if full)        |
        |                                                   |
        +---------------------> Pipeline::process <---------+
                                     |
                       OrderBook (single writer)
                                     |
                       MicrostructureCalculator
                                     |
                       MarketMakerStrategy -> QuoteDecision
                                     |
                       RiskEngine (pre-trade checks)
                                     |
                       PaperExecutionEngine (simulated fills)
                                     |
                       Portfolio (position / P&L)
                                     |
                       PipelineStats + LatencyStats -> end-of-run report
```

## Threads (live mode)

| Thread | Owns | Notes |
|---|---|---|
| WebSocket I/O (IXWebSocket internal) | `FinnhubClient` state, producer side of ring buffer | Parses JSON once, pushes `MarketEvent`s. Never blocks; on a full buffer it drops and increments a counter. |
| Main / consumer | `Pipeline` (order book, strategy, risk, execution, portfolio) | Single owner of all mutable trading state; no locks. |

Replay mode is single-threaded: `ReplayEngine` calls `Pipeline::process` directly.

## Data ownership

- `MarketEvent`: trivially copyable value, copied through the ring buffer (immutable after creation).
- `SpscRingBuffer` indices: atomics (release/acquire); each thread's cached copy of the other side's index is thread-local.
- `Pipeline` and everything it contains: single-owner, not thread-safe by design.
- `FinnhubClient` counters (`messages_received`, `reconnect_count`, ...): atomics read from the main thread.
- The Finnhub client's `last_trade_price_` / `last_side_` are only touched from the WebSocket thread.
