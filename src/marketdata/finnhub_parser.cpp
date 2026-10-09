#include "marketdata/finnhub_parser.hpp"

#include <nlohmann/json.hpp>

namespace lob {

FinnhubParseResult parse_finnhub_message(std::string_view payload) {
    FinnhubParseResult result;
    auto doc = nlohmann::json::parse(payload.begin(), payload.end(), nullptr, /*allow_exceptions=*/false);
    if (doc.is_discarded() || !doc.is_object()) return result;

    auto type_it = doc.find("type");
    if (type_it == doc.end() || !type_it->is_string()) return result;
    if (type_it->get_ref<const std::string&>() != "trade") {
        result.status = FinnhubParseStatus::Ignored;
        return result;
    }

    auto data_it = doc.find("data");
    if (data_it == doc.end() || !data_it->is_array()) return result;

    result.status = FinnhubParseStatus::Trades;
    for (const auto& tick : *data_it) {
        if (!tick.is_object() || !tick.contains("s") || !tick["s"].is_string() ||
            !tick.contains("p") || !tick["p"].is_number() || !tick.contains("v") ||
            !tick["v"].is_number() || !tick.contains("t") || !tick["t"].is_number_integer()) {
            ++result.malformed_ticks;
            continue;
        }
        double price = tick["p"].get<double>();
        if (!(price > 0.0)) {
            ++result.malformed_ticks;
            continue;
        }
        result.trades.push_back(FinnhubTrade{tick["s"].get<std::string>(), price,
                                              tick["v"].get<double>(), tick["t"].get<int64_t>()});
    }
    return result;
}

}  // namespace lob
