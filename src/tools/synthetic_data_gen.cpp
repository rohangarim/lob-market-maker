// Generates a deterministic, seeded synthetic L2 order-book event stream
// and writes it to the project's binary event-log format. This is NOT real
// market data -- it exists so the order book, strategy, risk, execution,
// portfolio, and replay/benchmark tooling can be exercised and measured
// end-to-end without depending on a paid historical-data subscription. See
// the README "What's real vs simulated" section.
//
// Model: a mid price random walk (Gaussian steps) with a resting ladder of
// kLevelsPerSide price levels on each side spaced by a configurable tick
// size. Each simulated step emits one event chosen from a weighted mix of
// quote updates (most common), trades, explicit level deletions, and
// occasional heartbeats, all tagged with strictly increasing sequence
// numbers and monotonically non-decreasing timestamps.

#include <cmath>
#include <iostream>
#include <random>
#include <vector>

#include "common/cli_args.hpp"
#include "marketdata/events.hpp"
#include "replay/event_codec.hpp"

using namespace lob;

namespace {

constexpr int kLevelsPerSide = 10;

struct SyntheticBookGenerator {
    std::mt19937_64 rng;
    double mid;
    double tick_size;
    Symbol symbol;
    SequenceNumber seq = 0;
    Timestamp t{std::chrono::seconds(1'700'000'000)};  // arbitrary fixed epoch, deterministic

    SyntheticBookGenerator(uint64_t seed, double start_price, double tick, Symbol sym)
        : rng(seed), mid(start_price), tick_size(tick), symbol(sym) {}

    MarketEvent next() {
        std::uniform_real_distribution<double> unit(0.0, 1.0);
        std::normal_distribution<double> walk(0.0, tick_size * 0.5);
        std::uniform_int_distribution<int> level_dist(0, kLevelsPerSide - 1);
        std::uniform_int_distribution<Quantity> qty_dist(1, 500);

        // Advance the reference mid price slightly on most steps.
        mid = std::max(1.0, mid + walk(rng));
        t += std::chrono::microseconds(std::uniform_int_distribution<int>(50, 2000)(rng));
        ++seq;

        double roll = unit(rng);
        if (roll < 0.05) {
            return MarketEvent::make_heartbeat(t, seq, symbol);
        }
        if (roll < 0.30) {
            // Trade near the touch.
            bool buy_aggressor = unit(rng) < 0.5;
            double px = buy_aggressor ? mid + tick_size * 0.5 : mid - tick_size * 0.5;
            return MarketEvent::make_trade(t, seq, symbol,
                                            buy_aggressor ? Side::Buy : Side::Sell,
                                            Price::from_double(px), qty_dist(rng));
        }
        if (roll < 0.40) {
            // Explicit level deletion.
            Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
            int level = level_dist(rng);
            double px = side == Side::Buy ? mid - tick_size * (level + 1)
                                           : mid + tick_size * (level + 1);
            return MarketEvent::make_delete(t, seq, symbol, side, Price::from_double(px));
        }
        // Quote update (add/modify a level), biased toward levels near the touch.
        Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
        int level = static_cast<int>(std::pow(unit(rng), 2.0) * kLevelsPerSide);
        level = std::min(level, kLevelsPerSide - 1);
        double px = side == Side::Buy ? mid - tick_size * (level + 1)
                                       : mid + tick_size * (level + 1);
        return MarketEvent::make_quote(t, seq, symbol, side, Price::from_double(px), qty_dist(rng));
    }
};

}  // namespace

int main(int argc, char** argv) {
    CliArgs args(argc, argv);
    std::string symbol_str = args.get_or("symbol", "AAPL");
    std::string out_path = args.get_or("out", "data/" + symbol_str + "_synthetic.bin");
    long num_events = args.get_long_or("events", 1'000'000);
    long seed = args.get_long_or("seed", 1234);
    double start_price = args.get_double_or("start-price", 190.00);
    double tick_size = args.get_double_or("tick-size", 0.01);

    std::cout << "Generating " << num_events << " synthetic L2 events for " << symbol_str
              << " (seed=" << seed << ", start_price=" << start_price << ") -> " << out_path
              << "\n";

    SyntheticBookGenerator gen(static_cast<uint64_t>(seed), start_price, tick_size,
                                Symbol(symbol_str));
    std::vector<MarketEvent> events;
    events.reserve(static_cast<size_t>(num_events));
    for (long i = 0; i < num_events; ++i) {
        events.push_back(gen.next());
    }

    write_events_to_file(out_path, events);
    std::cout << "Wrote " << events.size() << " events (" << (events.size() * kRecordSize + 8)
              << " bytes) to " << out_path << "\n";
    return 0;
}
