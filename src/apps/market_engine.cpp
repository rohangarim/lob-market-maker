// Main CLI entry point.
//
//   market_engine --symbol AAPL --mode replay --data data/AAPL_synthetic.bin --speed max
//   market_engine --symbol AAPL --mode live --config config/config.local.json --duration-sec 30
//
// Live and replay modes are built on the exact same Pipeline
// (src/common/pipeline.hpp); replay additionally supports deterministic,
// speed-controlled playback via ReplayEngine.

#include <algorithm>
#include <atomic>
#include <memory>
#include <stdexcept>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/cli_args.hpp"
#include "common/pipeline.hpp"
#include "config/pipeline_config_json.hpp"
#include "concurrency/spsc_ring_buffer.hpp"
#include "live/live_runner.hpp"
#include "replay/event_codec.hpp"
#include "replay/replay_engine.hpp"

#if LOB_BUILD_LIVE_FEED
#include "marketdata/finnhub_client.hpp"
#endif

using namespace lob;
using json = nlohmann::json;

namespace {

std::atomic<bool> g_stop{false};
void handle_sigint(int) { g_stop.store(true); }

double parse_speed(const std::string& s) {
    if (s == "max") return 0.0;
    std::string trimmed = s;
    if (!trimmed.empty() && (trimmed.back() == 'x' || trimmed.back() == 'X')) {
        trimmed.pop_back();
    }
    try {
        return std::stod(trimmed);
    } catch (...) {
        return 1.0;
    }
}

void print_latency(const char* label, const LatencyStats& lat) {
    if (lat.count() == 0) return;
    std::cout << "  " << label << " (n=" << lat.count() << "): p50 " << lat.percentile(50) / 1000.0
              << " us, p95 " << lat.percentile(95) / 1000.0 << " us, p99 "
              << lat.percentile(99) / 1000.0 << " us, max " << lat.max_ns() / 1000.0 << " us\n";
}

json latency_json(const LatencyStats& lat) {
    return {{"samples", lat.count()},
            {"p50_us", lat.percentile(50) / 1000.0},
            {"p95_us", lat.percentile(95) / 1000.0},
            {"p99_us", lat.percentile(99) / 1000.0},
            {"max_us", lat.max_ns() / 1000.0}};
}

json report_json(const Pipeline& pipeline, const char* mode_label, size_t ring_dropped) {
    const auto& st = pipeline.stats();
    const auto& pnl = pipeline.portfolio().state();
    return {{"mode", mode_label},
            {"events_processed", st.events_processed},
            {"events_dropped_ring_overflow", ring_dropped},
            {"out_of_order_dropped", st.out_of_order_dropped},
            {"sequence_gaps", st.sequence_gaps},
            {"stale_resets", st.stale_resets},
            {"clock_stale_events", st.clock_stale_events},
            {"crossed_book_events", st.crossed_book_events},
            {"orders_submitted", pipeline.execution().total_submitted()},
            {"orders_canceled", pipeline.execution().total_canceled()},
            {"fills", st.fills},
            {"partial_fills", st.partial_fills},
            {"risk_rejects", st.risk_rejects},
            {"position", pnl.position},
            {"realized_pnl", pnl.realized_pnl},
            {"unrealized_pnl", pnl.unrealized_pnl},
            {"total_pnl", pnl.total_pnl},
            {"latency_sample_interval", pipeline.latency_sample_interval()},
            {"decision_latency", latency_json(st.decision_latency)},
            {"book_update_latency", latency_json(st.book_update_latency)},
            {"execution_latency", latency_json(st.execution_latency)}};
}

void print_report(const Pipeline& pipeline, const char* mode_label, size_t ring_dropped = 0) {
    const auto& stats = pipeline.stats();
    const auto& pnl = pipeline.portfolio().state();
    const auto& exec = pipeline.execution();

    std::cout << "\n=== " << mode_label << " run report ===\n";
    std::cout << "events processed:     " << stats.events_processed << "\n";
    std::cout << "ring-buffer drops:    " << ring_dropped << "\n";
    std::cout << "out-of-order dropped: " << stats.out_of_order_dropped << "\n";
    std::cout << "sequence gaps:        " << stats.sequence_gaps << "\n";
    std::cout << "stale resets:         " << stats.stale_resets << " (+" << stats.clock_stale_events
              << " wall-clock stale)\n";
    std::cout << "crossed-book events:  " << stats.crossed_book_events << "\n";
    std::cout << "orders submitted:     " << exec.total_submitted() << "\n";
    std::cout << "orders canceled:      " << exec.total_canceled() << "\n";
    std::cout << "fills:                " << stats.fills << " (partial: " << stats.partial_fills << ")\n";
    std::cout << "risk rejects:         " << stats.risk_rejects << "\n";
    std::cout << "open orders:          " << exec.open_order_count() << "\n";
    std::cout << "-- latency (histogram buckets, lower bound) --\n";
    std::cout << "  (decision latency sampled every " << pipeline.latency_sample_interval() << " requotes)\n";
    print_latency("decision (strategy->risk->order)", stats.decision_latency);
    print_latency("book update + microstructure", stats.book_update_latency);
    print_latency("execution + mark-to-market", stats.execution_latency);
    std::cout << "-- position / P&L (simulated) --\n";
    std::cout << "  position:        " << pnl.position << "\n";
    std::cout << "  avg entry price: " << pnl.avg_entry_price << "\n";
    std::cout << "  realized P&L:    " << pnl.realized_pnl << "\n";
    std::cout << "  unrealized P&L:  " << pnl.unrealized_pnl << "\n";
    std::cout << "  total P&L:       " << pnl.total_pnl << "\n";
    std::cout << "  volume traded:   " << pnl.total_volume_traded << "\n";
    std::cout << "  turnover ($):    " << pnl.turnover << "\n";
}

PipelineConfig make_config(const CliArgs& args, const json& cfg) {
    PipelineConfig pcfg = build_pipeline_config(cfg);
    pcfg.profile_stages = args.has("profile-stages");
    if (args.has("latency-sample")) pcfg.latency_sample_interval = static_cast<uint32_t>(args.get_long_or("latency-sample", 16));
    return pcfg;
}

void maybe_write_json(const CliArgs& args, const json& report) {
    if (auto path = args.get("report-json")) {
        std::ofstream(*path) << report.dump(2) << "\n";
        std::cout << "JSON report written to " << *path << "\n";
    }
}

int run_replay(const CliArgs& args, const json& cfg, const std::string& symbol) {
    std::string data_path = args.get_or("data", "");
    if (data_path.empty()) {
        std::cerr << "error: --mode replay requires --data <path to .bin event log>\n";
        return 1;
    }
    double speed = parse_speed(args.get_or("speed", "max"));

    std::cout << "Loading events from " << data_path << "...\n";
    std::vector<MarketEvent> events = read_events_from_file(data_path);
    std::cout << "Loaded " << events.size() << " events. Replaying at speed="
              << (speed <= 0.0 ? "max" : std::to_string(speed) + "x") << "...\n";

    const long repeat = std::max(1L, args.get_long_or("repeat", 1));
    std::vector<double> rates;
    std::unique_ptr<ReplayEngine> engine;
    ReplaySummary summary;
    for (long r = 0; r < repeat; ++r) {
        engine = std::make_unique<ReplayEngine>(Symbol(symbol), make_config(args, cfg));
        summary = engine->run(events, speed);
        rates.push_back(summary.events_per_second / 1e6);
        if (repeat > 1) std::cout << "  run " << (r + 1) << ": " << rates.back() << " M events/sec\n";
    }

    std::cout << "\nReplay complete: " << summary.events_replayed << " events in "
              << (summary.wall_clock_duration.count() / 1e6) << " ms ("
              << (summary.events_per_second / 1e6) << " million events/sec)\n";
    if (repeat > 1) {
        std::sort(rates.begin(), rates.end());
        std::cout << "Across " << repeat << " runs (M events/sec): min " << rates.front() << ", median "
                  << rates[rates.size() / 2] << ", max " << rates.back() << "\n";
    }
    print_report(engine->pipeline(), "REPLAY");
    maybe_write_json(args, report_json(engine->pipeline(), "replay", 0));
    return 0;
}

int run_live(const CliArgs& args, const json& cfg, const std::string& symbol) {
#if LOB_BUILD_LIVE_FEED
    std::string api_key = cfg.value("finnhub_api_key", std::string());
    if (api_key.empty() || api_key.rfind("SET_VIA", 0) == 0) {
        if (const char* env = std::getenv("FINNHUB_API_KEY")) api_key = env;
    }
    if (api_key.empty()) {
        std::cerr << "error: no Finnhub API key found in config or FINNHUB_API_KEY env var\n";
        return 1;
    }
    long duration_sec = args.get_long_or("duration-sec", 60);
    double synthetic_spread = args.get_double_or("synthetic-spread", 0.02);

    Pipeline pipeline(Symbol(symbol), make_config(args, cfg));
    EventRing ring;
    std::atomic<size_t> dropped{0};

    // Producer: the WebSocket thread. Consumer (this thread): LiveRunner.
    FinnhubClient client(api_key, symbol, synthetic_spread, [&](const MarketEvent& ev) {
        if (!enqueue(ring, ev)) dropped.fetch_add(1, std::memory_order_relaxed);
    });

    std::unique_ptr<BinaryEventRecorder> recorder;
    if (auto path = args.get("record")) {
        recorder = std::make_unique<BinaryEventRecorder>(*path);
        std::cout << "Recording received events to " << *path << " (replayable with --mode replay)\n";
    }
    std::ofstream log_file;
    std::unique_ptr<StructuredLogger> logger;
    if (auto path = args.get("log-json")) {
        log_file.open(*path, std::ios::trunc);
        logger = std::make_unique<StructuredLogger>(log_file);
        logger->start();
        std::cout << "Structured JSON logs -> " << *path << "\n";
    }

    LiveRunner runner(pipeline, ring);
    runner.set_drop_counter(&dropped);
    runner.set_recorder(recorder.get());
    runner.set_logger(logger.get());
    runner.set_clock([] { return now_ns(); });

    std::cout << "Starting live Finnhub feed for " << symbol << " (Ctrl+C to stop early, or waits "
              << duration_sec << "s)...\n";
    std::cout << "NOTE: Finnhub's free tier streams trade prints only; top-of-book quotes are "
                 "synthesized around each trade (see marketdata/trade_tape_adapter.hpp).\n";

    std::signal(SIGINT, handle_sigint);
    std::atomic<bool> stop{false};
    client.start();

    std::thread consumer([&] { runner.run(stop); });
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "  connected=" << client.connected() << " msgs=" << client.messages_received()
                  << " malformed=" << client.malformed_messages() << " dropped=" << dropped.load()
                  << " reconnects=" << client.reconnect_count() << "\n";
    }
    client.stop();                 // producer is quiet from here on
    stop.store(true, std::memory_order_release);
    consumer.join();
    if (logger) logger->stop();

    const auto& q = runner.stats().queue_latency;
    std::cout << "\nqueue latency (enqueue -> dequeue, n=" << q.count() << "): p50 "
              << q.percentile(50) / 1000.0 << " us, p99 " << q.percentile(99) / 1000.0 << " us, max "
              << q.max_ns() / 1000.0 << " us\n";
    print_report(pipeline, "LIVE", dropped.load());
    maybe_write_json(args, report_json(pipeline, "live", dropped.load()));
    return 0;
#else
    (void)args; (void)cfg; (void)symbol;
    std::cerr << "error: this binary was built with BUILD_LIVE_FEED=OFF; live mode is unavailable.\n";
    return 1;
#endif
}

}  // namespace

int main(int argc, char** argv) {
    CliArgs args(argc, argv);
    std::string mode = args.get_or("mode", "replay");
    std::string symbol = args.get_or("symbol", "AAPL");
    std::string config_path = args.get_or("config", "config/config.local.json");

    json cfg = load_json_config(config_path);
    if (cfg.empty()) cfg = load_json_config("config/config.example.json");

    if (mode == "replay") {
        return run_replay(args, cfg, symbol);
    }
    if (mode == "live") {
        return run_live(args, cfg, symbol);
    }
    std::cerr << "error: unknown --mode '" << mode << "' (expected 'live' or 'replay')\n";
    return 1;
}
