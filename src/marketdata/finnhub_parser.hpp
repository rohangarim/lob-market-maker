#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace lob {

struct FinnhubTrade {
    std::string symbol;
    double price = 0.0;
    double volume = 0.0;
    int64_t timestamp_ms = 0;  // exchange timestamp, milliseconds since epoch
};

enum class FinnhubParseStatus : uint8_t {
    Trades,     // a trade message; `trades` holds every valid tick (possibly empty)
    Ignored,    // well-formed but not a trade (ping, error, ...)
    Malformed,  // not JSON, or missing/mistyped required fields
};

struct FinnhubParseResult {
    FinnhubParseStatus status = FinnhubParseStatus::Malformed;
    std::vector<FinnhubTrade> trades;
    size_t malformed_ticks = 0;  // ticks inside a trade message that were skipped
};

// Parses one WebSocket text frame in Finnhub's documented trade format:
//   {"type":"trade","data":[{"s":"AAPL","p":190.1,"v":100,"t":1700000000000,"c":[...]}]}
// Never throws. Invalid ticks are skipped and counted rather than aborting the
// whole message.
FinnhubParseResult parse_finnhub_message(std::string_view payload);

}  // namespace lob
