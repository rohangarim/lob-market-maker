// Generates a deterministic, seeded synthetic L2 order-book event stream and
// writes it to the project's binary event-log format. This is NOT real market
// data -- it exists so the order book, strategy, risk, execution, portfolio
// and replay/benchmark tooling can be exercised and measured end-to-end. The
// model is documented in tools/synthetic_book.hpp.

#include <iostream>
#include <vector>

#include "common/cli_args.hpp"
#include "replay/event_codec.hpp"
#include "tools/synthetic_book.hpp"

using namespace lob;

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

    SyntheticBookGenerator gen(static_cast<uint64_t>(seed), start_price, tick_size, Symbol(symbol_str));
    std::vector<MarketEvent> events;
    events.reserve(static_cast<size_t>(num_events));
    for (long i = 0; i < num_events; ++i) events.push_back(gen.next());

    write_events_to_file(out_path, events);
    std::cout << "Wrote " << events.size() << " events (" << (events.size() * kRecordSize + 8)
              << " bytes) to " << out_path << "\n";
    return 0;
}
