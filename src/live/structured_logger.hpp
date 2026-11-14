#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <ostream>
#include <thread>

#include <nlohmann/json.hpp>

#include "concurrency/spsc_ring_buffer.hpp"

namespace lob {

enum class LogKind : uint8_t { Stats = 0, Anomaly = 1 };
enum class AnomalyType : uint8_t {
    OutOfOrder = 0, SequenceGap, StaleReset, CrossedBook, ClockStale, RingDrop
};

inline const char* to_string(AnomalyType t) {
    switch (t) {
        case AnomalyType::OutOfOrder: return "out_of_order";
        case AnomalyType::SequenceGap: return "sequence_gap";
        case AnomalyType::StaleReset: return "stale_reset";
        case AnomalyType::CrossedBook: return "crossed_book";
        case AnomalyType::ClockStale: return "clock_stale";
        case AnomalyType::RingDrop: return "ring_drop";
    }
    return "unknown";
}

// Fixed-size, trivially copyable record: the hot thread only fills in numbers;
// all string formatting happens on the logger thread.
struct LogRecord {
    int64_t ts_ns = 0;
    LogKind kind = LogKind::Stats;
    AnomalyType anomaly = AnomalyType::OutOfOrder;
    uint64_t v[8] = {};   // Stats: events, dropped, gaps, out_of_order, stale, crossed, orders, fills
                          // Anomaly: v[0] = number of new occurrences since the last record
    double d[4] = {};     // Stats: position, pnl, queue_p99_us, decision_p99_us
};

// Writes one JSON object per line. Single producer (the consumer/pipeline thread,
// via log()), one internal writer thread. log() never blocks: if the 4096-slot ring
// is full the record is dropped and counted.
class StructuredLogger {
public:
    explicit StructuredLogger(std::ostream& out) : out_(out) {}
    ~StructuredLogger() { stop(); }
    StructuredLogger(const StructuredLogger&) = delete;
    StructuredLogger& operator=(const StructuredLogger&) = delete;

    void start() {
        if (running_.exchange(true)) return;
        stop_.store(false);
        thread_ = std::thread([this] { run(); });
    }

    // Drains everything already queued, then joins.
    void stop() {
        if (!running_.exchange(false)) return;
        stop_.store(true, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    bool log(const LogRecord& r) {
        if (ring_.push(r)) return true;
        dropped_.fetch_add(1, std::memory_order_relaxed);
        return false;
    }

    [[nodiscard]] size_t dropped() const noexcept { return dropped_.load(); }
    [[nodiscard]] size_t written() const noexcept { return written_.load(); }

private:
    void run() {
        while (true) {
            if (auto r = ring_.pop()) {
                write(*r);
            } else {
                out_.flush();
                if (stop_.load(std::memory_order_acquire) && ring_.empty()) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
        }
        out_.flush();
    }

    void write(const LogRecord& r) {
        nlohmann::json j;
        j["ts_ns"] = r.ts_ns;
        if (r.kind == LogKind::Stats) {
            j["kind"] = "stats";
            j["events"] = r.v[0];
            j["ring_dropped"] = r.v[1];
            j["sequence_gaps"] = r.v[2];
            j["out_of_order"] = r.v[3];
            j["stale_resets"] = r.v[4];
            j["crossed_book_events"] = r.v[5];
            j["orders"] = r.v[6];
            j["fills"] = r.v[7];
            j["position"] = r.d[0];
            j["pnl"] = r.d[1];
            j["queue_p99_us"] = r.d[2];
            j["decision_p99_us"] = r.d[3];
        } else {
            j["kind"] = "anomaly";
            j["type"] = to_string(r.anomaly);
            j["count"] = r.v[0];
        }
        out_ << j.dump() << '\n';
        written_.fetch_add(1, std::memory_order_relaxed);
    }

    std::ostream& out_;
    SpscRingBuffer<LogRecord, 4096> ring_;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> stop_{false};
    std::atomic<size_t> dropped_{0};
    std::atomic<size_t> written_{0};
};

}  // namespace lob
