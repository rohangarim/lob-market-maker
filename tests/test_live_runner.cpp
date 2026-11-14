#include <gtest/gtest.h>

#include <atomic>
#include <fstream>
#include <sstream>
#include <thread>

#include <nlohmann/json.hpp>

#include "live/live_runner.hpp"
#include "replay/replay_engine.hpp"
#include "test_util.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;
using lob_test::unique_temp_path;

namespace {

std::vector<MarketEvent> make_events(size_t n, double informed = 0.0) {
    SyntheticBookGenerator g(99, 190.0, 0.01, Symbol("AAPL"), 0.0, informed);
    std::vector<MarketEvent> ev;
    ev.reserve(n);
    for (size_t i = 0; i < n; ++i) ev.push_back(g.next());
    return ev;
}

PipelineConfig test_cfg() {
    PipelineConfig c;
    c.risk_limits.max_daily_loss = 1e9;
    c.strategy.base_half_spread_bps = 0.5;          // ~1 tick from mid, so fills actually happen
    c.strategy.inventory_skew_bps_per_unit = 0.01;
    return c;
}

struct Snapshot {
    Quantity position;
    double total_pnl, realized;
    size_t fills, orders, canceled, quotes, events;
};
Snapshot snap(const Pipeline& p) {
    return {p.portfolio().state().position, p.portfolio().state().total_pnl,
            p.portfolio().state().realized_pnl, p.stats().fills, p.execution().total_submitted(),
            p.execution().total_canceled(), p.stats().quotes_submitted, p.stats().events_processed};
}
void expect_same(const Snapshot& a, const Snapshot& b) {
    EXPECT_EQ(a.position, b.position);
    EXPECT_DOUBLE_EQ(a.total_pnl, b.total_pnl);
    EXPECT_DOUBLE_EQ(a.realized, b.realized);
    EXPECT_EQ(a.fills, b.fills);
    EXPECT_EQ(a.orders, b.orders);
    EXPECT_EQ(a.canceled, b.canceled);
    EXPECT_EQ(a.quotes, b.quotes);
    EXPECT_EQ(a.events, b.events);
}

// Producer thread that never drops: it spins when the ring is full.
void produce_all(EventRing& ring, const std::vector<MarketEvent>& events) {
    for (const auto& ev : events) {
        while (!enqueue(ring, ev)) std::this_thread::yield();
    }
}

std::vector<nlohmann::json> parse_lines(const std::string& text) {
    std::vector<nlohmann::json> out;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        out.push_back(nlohmann::json::parse(line));   // throws (fails the test) on invalid JSON
    }
    return out;
}

}  // namespace

// ---- recorder ----------------------------------------------------------------

TEST(EventRecorder, RoundTripsEveryEventExactly) {
    auto events = make_events(5000);
    std::string path = unique_temp_path();
    {
        BinaryEventRecorder rec(path);
        for (const auto& e : events) rec.record(e);
        EXPECT_EQ(rec.count(), events.size());
    }
    auto back = read_events_from_file(path);
    std::remove(path.c_str());
    ASSERT_EQ(back.size(), events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        ASSERT_EQ(back[i].sequence, events[i].sequence);
        ASSERT_EQ(back[i].timestamp, events[i].timestamp);
        ASSERT_EQ(back[i].type, events[i].type);
        ASSERT_EQ(back[i].price.ticks(), events[i].price.ticks());
        ASSERT_EQ(back[i].quantity, events[i].quantity);
        ASSERT_EQ(back[i].side, events[i].side);
    }
}

TEST(EventRecorder, UnwritablePathThrows) {
    EXPECT_THROW(BinaryEventRecorder("/nonexistent_dir_xyz/log.bin"), std::runtime_error);
}

// ---- structured logger -------------------------------------------------------------

TEST(StructuredLogger, WritesOneValidJsonLinePerRecordInOrder) {
    std::ostringstream out;
    {
        StructuredLogger log(out);
        log.start();
        for (int i = 0; i < 1000; ++i) {
            LogRecord r;
            r.ts_ns = i;
            r.kind = (i % 2) ? LogKind::Anomaly : LogKind::Stats;
            r.anomaly = AnomalyType::SequenceGap;
            r.v[0] = static_cast<uint64_t>(i);
            r.d[1] = i * 0.5;
            while (!log.log(r)) std::this_thread::yield();
        }
        log.stop();
        EXPECT_EQ(log.written(), 1000u);
    }
    auto lines = parse_lines(out.str());
    ASSERT_EQ(lines.size(), 1000u);
    for (int i = 0; i < 1000; ++i) {
        EXPECT_EQ(lines[i]["ts_ns"].get<int>(), i);
        if (i % 2) {
            EXPECT_EQ(lines[i]["kind"], "anomaly");
            EXPECT_EQ(lines[i]["type"], "sequence_gap");
            EXPECT_EQ(lines[i]["count"].get<int>(), i);
        } else {
            EXPECT_EQ(lines[i]["kind"], "stats");
            EXPECT_EQ(lines[i]["events"].get<int>(), i);
            EXPECT_DOUBLE_EQ(lines[i]["pnl"].get<double>(), i * 0.5);
        }
    }
}

TEST(StructuredLogger, FullRingDropsAndCountsInsteadOfBlocking) {
    std::ostringstream out;
    StructuredLogger log(out);          // not started: nothing drains the ring
    size_t accepted = 0;
    for (int i = 0; i < 5000; ++i) accepted += log.log(LogRecord{}) ? 1 : 0;
    EXPECT_EQ(accepted, 4095u);
    EXPECT_EQ(log.dropped(), 5000u - 4095u);
    log.start();
    log.stop();
    EXPECT_EQ(log.written(), 4095u);    // stop() drains what was queued
}

TEST(StructuredLogger, StopWithoutStartAndDoubleStopAreSafe) {
    std::ostringstream out;
    StructuredLogger log(out);
    log.stop();
    log.start();
    log.start();
    log.stop();
    log.stop();
}

// ---- live runner ---------------------------------------------------------------------

TEST(LiveRunner, RecordedLiveSessionReplaysToExactlyTheSameState) {
    // More events than the ring holds, so the producer genuinely has to wait.
    auto events = make_events(250'000, 0.5);
    std::string path = unique_temp_path();

    Pipeline live(Symbol("AAPL"), test_cfg());
    EventRing ring;
    BinaryEventRecorder recorder(path);
    LiveRunner runner(live, ring);
    runner.set_recorder(&recorder);

    std::atomic<bool> stop{false};
    std::thread consumer([&] { runner.run(stop); });
    std::thread producer([&] { produce_all(ring, events); });
    producer.join();
    stop.store(true, std::memory_order_release);
    consumer.join();

    EXPECT_EQ(runner.stats().dequeued, events.size());
    EXPECT_EQ(recorder.count(), events.size());

    ReplayEngine replay(Symbol("AAPL"), test_cfg());
    replay.run(read_events_from_file(path), 0.0);
    std::remove(path.c_str());

    Pipeline direct(Symbol("AAPL"), test_cfg());
    for (const auto& e : events) direct.process(e);

    EXPECT_GT(snap(live).fills, 100u);
    expect_same(snap(live), snap(replay.pipeline()));
    expect_same(snap(live), snap(direct));
}

TEST(LiveRunner, MeasuresQueueLatencyForEveryEvent) {
    auto events = make_events(20'000);
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    LiveRunner runner(p, ring);
    std::atomic<bool> stop{false};
    std::thread consumer([&] { runner.run(stop); });
    produce_all(ring, events);
    stop.store(true, std::memory_order_release);
    consumer.join();
    EXPECT_EQ(runner.stats().queue_latency.count(), events.size());
    EXPECT_GE(runner.stats().queue_latency.max_ns(), runner.stats().queue_latency.percentile(50));
}

TEST(LiveRunner, ProcessesEverythingQueuedBeforeStopIsSet) {
    auto events = make_events(30'000);
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    for (size_t i = 0; i < 30'000 && i < EventRing::capacity(); ++i) ASSERT_TRUE(enqueue(ring, events[i]));
    LiveRunner runner(p, ring);
    std::atomic<bool> stop{true};          // already stopping: must still drain, then exit
    runner.run(stop);
    EXPECT_EQ(runner.stats().dequeued, 30'000u);
    EXPECT_TRUE(ring.empty());
}

TEST(LiveRunner, SilentFeedTriggersClockStaleAndIsLogged) {
    auto events = make_events(5'000);
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    std::ostringstream out;
    StructuredLogger logger(out);
    logger.start();
    LiveRunner runner(p, ring);
    runner.set_logger(&logger);
    // Wall clock that is far past the (synthetic, year-2023) event timestamps.
    runner.set_clock([] { return Timestamp(std::chrono::seconds(2'000'000'000)); });
    for (const auto& e : events) ASSERT_TRUE(enqueue(ring, e));
    std::atomic<bool> stop{true};
    runner.run(stop);
    logger.stop();
    EXPECT_TRUE(p.market_data_stale());
    EXPECT_EQ(p.execution().open_order_count(), 0u);
    // The flag can trip more than once: events still in the ring after the first
    // trip are fresh data and clear it, then the next clock check trips it again.
    EXPECT_GE(p.stats().clock_stale_events, 1u);

    uint64_t logged = 0;
    for (auto& j : parse_lines(out.str()))
        if (j["kind"] == "anomaly" && j["type"] == "clock_stale") logged += j["count"].get<uint64_t>();
    EXPECT_EQ(logged, p.stats().clock_stale_events);
}

TEST(LiveRunner, DuplicateAndOutOfOrderEventsAreCountedAndLogged) {
    auto events = make_events(2'000);
    std::vector<MarketEvent> noisy;
    size_t injected = 0;
    for (size_t i = 0; i < events.size(); ++i) {
        noisy.push_back(events[i]);
        if (i % 100 == 50) { noisy.push_back(events[i]); ++injected; }   // exact duplicate
    }
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    std::ostringstream out;
    StructuredLogger logger(out);
    logger.start();
    LiveRunner runner(p, ring);
    runner.set_logger(&logger);
    for (const auto& e : noisy) ASSERT_TRUE(enqueue(ring, e));
    std::atomic<bool> stop{true};
    runner.run(stop);
    logger.stop();

    EXPECT_EQ(p.stats().out_of_order_dropped, injected);
    EXPECT_EQ(p.stats().events_processed, noisy.size());
    uint64_t logged = 0;
    for (auto& j : parse_lines(out.str()))
        if (j["kind"] == "anomaly" && j["type"] == "out_of_order") logged += j["count"].get<uint64_t>();
    EXPECT_EQ(logged, injected);
}

TEST(LiveRunner, ProducerSideDropsAreReportedAsRingDropAnomalies) {
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    std::atomic<size_t> drops{7};
    std::ostringstream out;
    StructuredLogger logger(out);
    logger.start();
    LiveRunner runner(p, ring);
    runner.set_logger(&logger);
    runner.set_drop_counter(&drops);
    std::atomic<bool> stop{true};
    runner.run(stop);
    logger.stop();
    uint64_t total = 0, stats_dropped = 0;
    for (auto& j : parse_lines(out.str())) {
        if (j["kind"] == "anomaly" && j["type"] == "ring_drop") total += j["count"].get<uint64_t>();
        if (j["kind"] == "stats") stats_dropped = j["ring_dropped"].get<uint64_t>();
    }
    EXPECT_EQ(total, 7u);
    EXPECT_EQ(stats_dropped, 7u);
}

TEST(LiveRunner, FinalStatsRecordIsAlwaysWritten) {
    auto events = make_events(1'000);
    Pipeline p(Symbol("AAPL"), test_cfg());
    EventRing ring;
    std::ostringstream out;
    StructuredLogger logger(out);
    logger.start();
    LiveRunner runner(p, ring);
    runner.set_logger(&logger);
    for (const auto& e : events) ASSERT_TRUE(enqueue(ring, e));
    std::atomic<bool> stop{true};
    runner.run(stop);
    logger.stop();
    auto lines = parse_lines(out.str());
    ASSERT_FALSE(lines.empty());
    EXPECT_EQ(lines.back()["kind"], "stats");
    EXPECT_EQ(lines.back()["events"].get<size_t>(), events.size());
}
