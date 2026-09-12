// Main CLI entry point.
//
//   market_engine --symbol AAPL --mode replay --data data/AAPL_synthetic.bin --speed max
//   market_engine --symbol AAPL --mode live --config config/config.local.json --duration-sec 30
//
// Live and replay modes are built on the exact same Pipeline
// (src/common/pipeline.hpp); replay additionally supports deterministic,
// speed-controlled playback via ReplayEngine.

#include <atomic>
#include <csignal>
#include <fstream>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>

#include "common/cli_args.hpp"
#include "common/pipeline.hpp"
#include "concurrency/spsc_ring_buffer.hpp"
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

json load_config(const std::string& path) {
    std::ifstream ifs(path);
    if (!ifs) {
        std::cerr << "note: config file '" << path << "' not found, using built-in defaults\n";
        return json::object();
    }
    json j;
    ifs >> j;
    return j;
}

PipelineConfig build_pipeline_config(const json& cfg) {
    PipelineConfig pcfg;
    if (cfg.contains("risk")) {
        auto& r = cfg["risk"];
        if (r.contains("max_position")) pcfg.risk_limits.max_position = r["max_position"].get<Quantity>();
        if (r.contains("max_order_size")) pcfg.risk_limits.max_order_size = r["max_order_size"].get<Quantity>();
        if (r.contains("max_notional")) pcfg.risk_limits.max_notional = r["max_notional"].get<double>();
        if (r.contains("max_daily_loss")) pcfg.risk_limits.max_daily_loss = r["max_daily_loss"].get<double>();
        if (r.contains("max_open_orders")) pcfg.risk_limits.max_open_orders = r["max_open_orders"].get<size_t>();
    }
    if (cfg.contains("strategy")) {
        auto& s = cfg["strategy"];
        if (s.contains("base_half_spread_bps")) pcfg.strategy.base_half_spread_bps = s["base_half_spread_bps"].get<double>();
        if (s.contains("quote_size")) pcfg.strategy.quote_size = s["quote_size"].get<Quantity>();
        if (s.contains("inventory_skew_bps_per_unit")) pcfg.strategy.inventory_skew_bps_per_unit = s["inventory_skew_bps_per_unit"].get<double>();
        if (s.contains("max_inventory_for_skew")) pcfg.strategy.max_inventory_for_skew = s["max_inventory_for_skew"].get<Quantity>();
        if (s.contains("volatility_spread_multiplier")) pcfg.strategy.volatility_spread_multiplier = s["volatility_spread_multiplier"].get<double>();
    }
    return pcfg;
}

void print_report(const Pipeline& pipeline, const char* mode_label) {
    const auto& stats = pipeline.stats();
    const auto& pnl = pipeline.portfolio().state();
    const auto& exec = pipeline.execution();

    std::cout << "\n=== " << mode_label << " run report ===\n";
    std::cout << "events processed:     " << stats.events_processed << "\n";
    std::cout << "sequence gaps:        " << stats.sequence_gaps << "\n";
    std::cout << "quotes submitted:     " << stats.quotes_submitted << "\n";
    std::cout << "quotes canceled:      " << stats.quotes_canceled << "\n";
    std::cout << "fills:                " << stats.fills << " (partial: " << stats.partial_fills << ")\n";
    std::cout << "risk rejects:         " << stats.risk_rejects << "\n";
    std::cout << "open orders:          " << exec.open_order_count() << "\n";
    std::cout << "-- decision latency (signal -> strategy -> risk -> order) --\n";
    if (stats.decision_latency.count() > 0) {
        std::cout << "  samples: " << stats.decision_latency.count() << "\n";
        std::cout << "  p50: " << stats.decision_latency.percentile(50) / 1000.0 << " us\n";
        std::cout << "  p95: " << stats.decision_latency.percentile(95) / 1000.0 << " us\n";
        std::cout << "  p99: " << stats.decision_latency.percentile(99) / 1000.0 << " us\n";
        std::cout << "  max: " << stats.decision_latency.max_ns() / 1000.0 << " us\n";
    } else {
        std::cout << "  (no quoting decisions were made)\n";
    }
    std::cout << "-- position / P&L --\n";
    std::cout << "  position:        " << pnl.position << "\n";
    std::cout << "  avg entry price: " << pnl.avg_entry_price << "\n";
    std::cout << "  realized P&L:    " << pnl.realized_pnl << "\n";
    std::cout << "  unrealized P&L:  " << pnl.unrealized_pnl << "\n";
    std::cout << "  total P&L:       " << pnl.total_pnl << "\n";
    std::cout << "  volume traded:   " << pnl.total_volume_traded << "\n";
    std::cout << "  turnover ($):    " << pnl.turnover << "\n";
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

    ReplayEngine engine(Symbol(symbol), build_pipeline_config(cfg));
    ReplaySummary summary = engine.run(events, speed);

    std::cout << "\nReplay complete: " << summary.events_replayed << " events in "
              << (summary.wall_clock_duration.count() / 1e6) << " ms ("
              << (summary.events_per_second / 1e6) << " million events/sec)\n";
    print_report(engine.pipeline(), "REPLAY");
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

    Pipeline pipeline(Symbol(symbol), build_pipeline_config(cfg));

    // Bounded SPSC ring buffer between the WebSocket I/O thread (producer,
    // driven by IXWebSocket's own callback thread) and this consumer loop
    // (single owner of the Pipeline/order book).
    SpscRingBuffer<MarketEvent, 1 << 16> ring;
    std::atomic<size_t> dropped{0};

    FinnhubClient client(api_key, symbol, synthetic_spread, [&](const MarketEvent& ev) {
        if (!ring.push(ev)) dropped.fetch_add(1, std::memory_order_relaxed);
    });

    std::cout << "Starting live Finnhub feed for " << symbol << " (Ctrl+C to stop early, "
              << "or waits " << duration_sec << "s)...\n";
    std::cout << "NOTE: Finnhub's free tier streams trade prints only; top-of-book quotes are "
                 "synthesized around each trade (see marketdata/finnhub_client.hpp).\n";

    std::signal(SIGINT, handle_sigint);
    client.start();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    auto last_report = std::chrono::steady_clock::now();
    while (!g_stop.load() && std::chrono::steady_clock::now() < deadline) {
        while (auto ev = ring.pop()) {
            pipeline.process(*ev);
        }
        if (std::chrono::steady_clock::now() - last_report > std::chrono::seconds(2)) {
            std::cout << "\r  connected=" << client.connected()
                      << " msgs=" << client.messages_received()
                      << " processed=" << pipeline.stats().events_processed
                      << " dropped=" << dropped.load()
                      << " reconnects=" << client.reconnect_count()
                      << " position=" << pipeline.portfolio().state().position
                      << " pnl=" << pipeline.portfolio().state().total_pnl << std::flush;
            last_report = std::chrono::steady_clock::now();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::cout << "\n";
    client.stop();
    while (auto ev = ring.pop()) pipeline.process(*ev);  // drain remaining

    print_report(pipeline, "LIVE");
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

    json cfg = load_config(config_path);
    if (cfg.empty()) cfg = load_config("config/config.example.json");

    if (mode == "replay") {
        return run_replay(args, cfg, symbol);
    }
    if (mode == "live") {
        return run_live(args, cfg, symbol);
    }
    std::cerr << "error: unknown --mode '" << mode << "' (expected 'live' or 'replay')\n";
    return 1;
}
