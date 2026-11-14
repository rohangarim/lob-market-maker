#pragma once

#include <atomic>
#include <chrono>
#include <functional>
#include <thread>

#include "common/pipeline.hpp"
#include "live/event_recorder.hpp"
#include "live/queued_event.hpp"
#include "live/structured_logger.hpp"
#include "metrics/latency_stats.hpp"

namespace lob {

struct LiveRunnerConfig {
    std::chrono::microseconds idle_sleep{100};      // how long to nap when the ring is empty
    std::chrono::milliseconds log_interval{1000};   // wall-clock period of stats records
};

struct LiveRunnerStats {
    size_t dequeued = 0;
    LatencyStats queue_latency;  // enqueue -> dequeue, nanoseconds
};

// ---------------------------------------------------------------------------
// The consumer side of live mode: drains the SPSC ring, optionally records each
// event, runs it through the Pipeline, feeds wall-clock time to on_clock() so a
// silent feed is noticed, measures how long events waited in the queue, and emits
// structured log records. Everything here runs on ONE thread (the consumer); the
// pipeline, recorder and stats have no other owner. The producer only calls
// enqueue(), and the optional drop counter is the only atomic shared with it.
// ---------------------------------------------------------------------------
class LiveRunner {
public:
    LiveRunner(Pipeline& pipeline, EventRing& ring, LiveRunnerConfig cfg = {})
        : pipeline_(pipeline), ring_(ring), cfg_(cfg) {}

    void set_recorder(BinaryEventRecorder* r) { recorder_ = r; }
    void set_logger(StructuredLogger* l) { logger_ = l; }
    void set_drop_counter(const std::atomic<size_t>* c) { dropped_ = c; }
    // Returns "now" in the same epoch as event timestamps; unset disables on_clock().
    void set_clock(std::function<Timestamp()> clock) { clock_ = std::move(clock); }

    // Runs until `stop` is set AND the ring is empty. Set `stop` only after the
    // producer has finished pushing, otherwise late events could be left behind.
    void run(const std::atomic<bool>& stop) {
        auto last_log = std::chrono::steady_clock::now();
        while (true) {
            size_t n = drain();
            if (clock_) pipeline_.on_clock(clock_());
            report_anomalies();
            auto now = std::chrono::steady_clock::now();
            if (now - last_log >= cfg_.log_interval) {
                log_stats();
                last_log = now;
            }
            if (n == 0) {
                if (stop.load(std::memory_order_acquire)) {
                    if (ring_.empty()) break;
                    continue;
                }
                std::this_thread::sleep_for(cfg_.idle_sleep);
            }
        }
        report_anomalies();
        log_stats();
        if (recorder_) recorder_->flush();
    }

    [[nodiscard]] const LiveRunnerStats& stats() const noexcept { return stats_; }

private:
    size_t drain() {
        size_t n = 0;
        while (n < 4096) {
            auto q = ring_.pop();
            if (!q) break;
            stats_.queue_latency.add_ns(steady_now_ns() - q->enqueue_steady_ns);
            if (recorder_) recorder_->record(q->event);
            pipeline_.process(q->event);
            ++stats_.dequeued;
            ++n;
        }
        return n;
    }

    void emit_anomaly(AnomalyType type, size_t delta) {
        if (!logger_ || delta == 0) return;
        LogRecord r;
        r.ts_ns = now_ns().count();
        r.kind = LogKind::Anomaly;
        r.anomaly = type;
        r.v[0] = delta;
        logger_->log(r);
    }

    void report_anomalies() {
        const auto& s = pipeline_.stats();
        emit_anomaly(AnomalyType::OutOfOrder, s.out_of_order_dropped - seen_.out_of_order_dropped);
        emit_anomaly(AnomalyType::SequenceGap, s.sequence_gaps - seen_.sequence_gaps);
        emit_anomaly(AnomalyType::StaleReset, s.stale_resets - seen_.stale_resets);
        emit_anomaly(AnomalyType::CrossedBook, s.crossed_book_events - seen_.crossed_book_events);
        emit_anomaly(AnomalyType::ClockStale, s.clock_stale_events - seen_.clock_stale_events);
        if (dropped_) {
            size_t d = dropped_->load(std::memory_order_relaxed);
            emit_anomaly(AnomalyType::RingDrop, d - seen_drops_);
            seen_drops_ = d;
        }
        seen_.out_of_order_dropped = s.out_of_order_dropped;
        seen_.sequence_gaps = s.sequence_gaps;
        seen_.stale_resets = s.stale_resets;
        seen_.crossed_book_events = s.crossed_book_events;
        seen_.clock_stale_events = s.clock_stale_events;
    }

    void log_stats() {
        if (!logger_) return;
        const auto& s = pipeline_.stats();
        LogRecord r;
        r.ts_ns = now_ns().count();
        r.kind = LogKind::Stats;
        r.v[0] = s.events_processed;
        r.v[1] = dropped_ ? dropped_->load(std::memory_order_relaxed) : 0;
        r.v[2] = s.sequence_gaps;
        r.v[3] = s.out_of_order_dropped;
        r.v[4] = s.stale_resets;
        r.v[5] = s.crossed_book_events;
        r.v[6] = pipeline_.execution().total_submitted();
        r.v[7] = s.fills;
        r.d[0] = static_cast<double>(pipeline_.portfolio().state().position);
        r.d[1] = pipeline_.portfolio().state().total_pnl;
        r.d[2] = stats_.queue_latency.percentile(99) / 1000.0;
        r.d[3] = s.decision_latency.percentile(99) / 1000.0;
        logger_->log(r);
    }

    Pipeline& pipeline_;
    EventRing& ring_;
    LiveRunnerConfig cfg_;
    BinaryEventRecorder* recorder_ = nullptr;
    StructuredLogger* logger_ = nullptr;
    const std::atomic<size_t>* dropped_ = nullptr;
    std::function<Timestamp()> clock_;
    LiveRunnerStats stats_;
    PipelineStats seen_;
    size_t seen_drops_ = 0;
};

}  // namespace lob
