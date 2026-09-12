#pragma once

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "common/pipeline.hpp"
#include "replay/event_codec.hpp"

namespace lob {

struct ReplaySummary {
    size_t events_replayed = 0;
    std::chrono::nanoseconds wall_clock_duration{0};
    double events_per_second = 0.0;
};

// ---------------------------------------------------------------------------
// Deterministic historical replay.
//
// Determinism: given the same binary event log and the same PipelineConfig
// (including the execution engine's RNG seed), replay produces bit-for-bit
// identical Pipeline state (order book, portfolio, fill sequence) on every
// run. This holds because:
//   - events are read from the log strictly in file order (the order they
//     were recorded, which preserves the original sequence-number order),
//   - the only "clock" used to decide behavior is the timestamp *carried in
//     each event*, never the wall-clock time replay happens to run at,
//   - the execution engine's randomness (partial-fill decisions) comes
//     from a seeded PRNG consumed in a fixed order, not from any
//     nondeterministic source (thread scheduling never affects results
///     because Pipeline::process is called from a single thread here).
//
// --speed controls only *pacing* (how fast wall-clock time advances
// relative to event timestamps) for human-watchable demos; it has zero
// effect on the sequence of decisions made, so results are identical
// across speeds. speed <= 0 means "max": no pacing delay at all, used for
// throughput benchmarking.
// ---------------------------------------------------------------------------
class ReplayEngine {
public:
    ReplayEngine(Symbol symbol, PipelineConfig cfg) : pipeline_(symbol, std::move(cfg)) {}

    ReplaySummary run(const std::vector<MarketEvent>& events, double speed = 0.0) {
        ReplaySummary summary;
        auto start = std::chrono::steady_clock::now();

        std::optional<Timestamp> last_event_ts;
        for (const auto& ev : events) {
            if (speed > 0.0 && last_event_ts) {
                auto delta = ev.timestamp - *last_event_ts;
                if (delta.count() > 0) {
                    auto scaled = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::duration<double, std::nano>(static_cast<double>(delta.count()) / speed));
                    std::this_thread::sleep_for(scaled);
                }
            }
            last_event_ts = ev.timestamp;
            pipeline_.process(ev);
            ++summary.events_replayed;
        }

        auto end = std::chrono::steady_clock::now();
        summary.wall_clock_duration = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start);
        double secs = static_cast<double>(summary.wall_clock_duration.count()) / 1e9;
        summary.events_per_second = secs > 0.0 ? static_cast<double>(summary.events_replayed) / secs : 0.0;
        return summary;
    }

    [[nodiscard]] const Pipeline& pipeline() const noexcept { return pipeline_; }

private:
    Pipeline pipeline_;
};

}  // namespace lob
