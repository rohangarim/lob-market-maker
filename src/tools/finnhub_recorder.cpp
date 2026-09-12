// Connects to the real Finnhub WebSocket trade stream and records the
// converted MarketEvents to the project's binary event-log format. This is
// the tool that proves live ingestion actually works against a real
// provider; the resulting file can then be replayed deterministically with
// market_engine --mode replay.

#include <chrono>
#include <iostream>
#include <mutex>
#include <thread>
#include <vector>

#include "common/cli_args.hpp"
#include "marketdata/finnhub_client.hpp"
#include "replay/event_codec.hpp"

using namespace lob;

int main(int argc, char** argv) {
    CliArgs args(argc, argv);
    std::string api_key = args.get_or("api-key", "");
    std::string symbol = args.get_or("symbol", "AAPL");
    std::string out_path = args.get_or("out", "data/" + symbol + "_live.bin");
    long duration_sec = args.get_long_or("duration-sec", 30);
    double synthetic_spread = args.get_double_or("synthetic-spread", 0.02);

    if (api_key.empty()) {
        if (const char* env = std::getenv("FINNHUB_API_KEY")) api_key = env;
    }
    if (api_key.empty()) {
        std::cerr << "error: pass --api-key or set FINNHUB_API_KEY\n";
        return 1;
    }

    std::mutex mu;
    std::vector<MarketEvent> events;

    FinnhubClient client(api_key, symbol, synthetic_spread, [&](const MarketEvent& ev) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(ev);
    });

    std::cout << "Connecting to Finnhub for " << symbol << ", recording for " << duration_sec
              << "s...\n";
    client.start();

    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(duration_sec);
    while (std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        std::lock_guard<std::mutex> lock(mu);
        std::cout << "\r  connected=" << client.connected()
                  << " messages=" << client.messages_received()
                  << " events=" << events.size()
                  << " malformed=" << client.malformed_messages()
                  << " reconnects=" << client.reconnect_count() << std::flush;
    }
    std::cout << "\n";
    client.stop();

    std::lock_guard<std::mutex> lock(mu);
    if (events.empty()) {
        std::cerr << "warning: no events recorded (market may be closed, or symbol has no "
                     "trade activity right now) -- no file written\n";
        return 1;
    }
    write_events_to_file(out_path, events);
    std::cout << "Wrote " << events.size() << " real Finnhub-derived events to " << out_path
              << "\n";
    return 0;
}
