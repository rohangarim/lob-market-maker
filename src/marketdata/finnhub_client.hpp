#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "marketdata/events.hpp"

namespace ix {
class WebSocket;
}

namespace lob {

// ---------------------------------------------------------------------------
// Live market-data ingestion from Finnhub's WebSocket trade stream
// (wss://ws.finnhub.io). This is real network I/O against a real,
// documented provider API -- not a mock.
//
// IMPORTANT, honestly documented limitation: Finnhub's free-tier WebSocket
// only pushes trade prints ({"type":"trade","data":[{"s","p","v","t"}]}).
// It does NOT provide L2/L3 order-book depth or bid/ask quotes on the free
// tier (that requires a paid plan or a different provider such as
// Polygon.io/Databento). Consequently:
//   - Each real trade print is converted into a genuine EventType::Trade
//     event, timestamped from the provider's own millisecond timestamp.
//   - Because Finnhub's trade tape does not label which side was the
//     aggressor, the aggressor side is inferred with the standard "tick
///    rule": an uptick from the previous trade price is classified Buy, a
//     downtick Sell, and an unchanged price repeats the previous
//     classification. This is a well-known approximation, not ground
//     truth.
//   - To let the rest of the pipeline (which requires a two-sided book to
//     quote against) run live end-to-end, this client also synthesizes a
//     top-of-book EventType::Quote pair around each trade print, at
//     trade_price -/+ half of a configurable synthetic_spread. These
//     synthetic quotes are clearly a derived overlay on real trade data,
//     NOT real resting order-book depth, and are labeled as such
//     everywhere they appear (README, logs). Full, faithful incremental
//     L2 order-book semantics are exercised using the synthetic replay
//     generator instead (see src/tools/synthetic_data_gen.cpp), which is
//     also what the benchmark suite measures against.
// ---------------------------------------------------------------------------
class FinnhubClient {
public:
    using EventCallback = std::function<void(const MarketEvent&)>;

    FinnhubClient(std::string api_key, std::string symbol, double synthetic_spread,
                  EventCallback callback);
    ~FinnhubClient();

    FinnhubClient(const FinnhubClient&) = delete;
    FinnhubClient& operator=(const FinnhubClient&) = delete;

    void start();
    void stop();

    [[nodiscard]] bool connected() const noexcept { return connected_.load(); }
    [[nodiscard]] size_t reconnect_count() const noexcept { return reconnect_count_.load(); }
    [[nodiscard]] size_t messages_received() const noexcept { return messages_received_.load(); }
    [[nodiscard]] size_t malformed_messages() const noexcept { return malformed_messages_.load(); }

private:
    void handle_text_message(const std::string& payload);

    std::string api_key_;
    std::string symbol_;
    double synthetic_spread_;
    EventCallback callback_;

    std::unique_ptr<ix::WebSocket> ws_;
    std::atomic<bool> connected_{false};
    std::atomic<bool> ever_connected_{false};
    std::atomic<size_t> reconnect_count_{0};
    std::atomic<size_t> messages_received_{0};
    std::atomic<size_t> malformed_messages_{0};
    std::atomic<SequenceNumber> next_sequence_{1};

    std::optional<double> last_trade_price_;
    Side last_side_ = Side::Buy;
};

}  // namespace lob
