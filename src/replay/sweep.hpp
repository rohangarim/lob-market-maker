#pragma once

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "replay/replay_engine.hpp"

namespace lob {

struct SweepPoint {
    std::string label;
    PipelineConfig config;
};

struct SweepResult {
    std::string label;
    size_t events = 0;
    size_t orders_submitted = 0;
    size_t orders_canceled = 0;
    size_t fills = 0;
    size_t taker_fills = 0;
    size_t risk_rejects = 0;
    Quantity final_position = 0;
    Quantity max_abs_position = 0;
    double mean_abs_position = 0.0;
    double realized_pnl = 0.0;
    double fees_paid = 0.0;
    double total_pnl = 0.0;
    double max_drawdown = 0.0;
    double filled_qty = 0.0;
    double avg_edge = 0.0;                 // per share, captured at the fill
    std::vector<double> avg_markout;       // per share, one entry per markout horizon
    std::vector<double> markout_qty;       // shares marked at each horizon
    double events_per_second = 0.0;        // wall-clock; the only non-deterministic field
};

inline SweepResult run_one(const std::vector<MarketEvent>& events, Symbol symbol, const SweepPoint& pt) {
    ReplayEngine engine(symbol, pt.config);
    ReplaySummary summary = engine.run(events, 0.0);
    const Pipeline& p = engine.pipeline();
    const auto& st = p.stats();
    const auto& pnl = p.portfolio().state();
    const auto& mk = p.markout();

    SweepResult r;
    r.label = pt.label;
    r.events = st.events_processed;
    r.orders_submitted = p.execution().total_submitted();
    r.orders_canceled = p.execution().total_canceled();
    r.fills = st.fills;
    r.taker_fills = p.execution().total_taker_fills();
    r.risk_rejects = st.risk_rejects;
    r.final_position = pnl.position;
    r.max_abs_position = p.analytics().max_abs_position;
    r.mean_abs_position = p.analytics().mean_abs_position();
    r.realized_pnl = pnl.realized_pnl;
    r.fees_paid = pnl.fees_paid;
    r.total_pnl = pnl.total_pnl;
    r.max_drawdown = p.analytics().max_drawdown;
    r.filled_qty = mk.filled_qty();
    r.avg_edge = mk.avg_edge();
    for (size_t h = 0; h < mk.horizon_count(); ++h) {
        r.avg_markout.push_back(mk.avg_markout(h));
        r.markout_qty.push_back(mk.marked_qty(h));
    }
    r.events_per_second = summary.events_per_second;
    return r;
}

// Replays every point over the same immutable event log. Each point gets its own
// Pipeline (no shared mutable state), and results land in a pre-sized vector by
// index, so the output is identical for any thread count and any scheduling.
inline std::vector<SweepResult> run_sweep(const std::vector<MarketEvent>& events, Symbol symbol,
                                          const std::vector<SweepPoint>& points, size_t threads) {
    std::vector<SweepResult> results(points.size());
    if (points.empty()) return results;
    threads = std::max<size_t>(1, std::min(threads, points.size()));

    std::atomic<size_t> next{0};
    auto worker = [&] {
        for (size_t i = next.fetch_add(1); i < points.size(); i = next.fetch_add(1)) {
            results[i] = run_one(events, symbol, points[i]);
        }
    };
    if (threads == 1) {
        worker();
    } else {
        std::vector<std::thread> pool;
        pool.reserve(threads);
        for (size_t t = 0; t < threads; ++t) pool.emplace_back(worker);
        for (auto& th : pool) th.join();
    }
    return results;
}

}  // namespace lob
