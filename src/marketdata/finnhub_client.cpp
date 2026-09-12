#include "marketdata/finnhub_client.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <iostream>

namespace lob {

using json = nlohmann::json;

FinnhubClient::FinnhubClient(std::string api_key, std::string symbol, double synthetic_spread,
                              EventCallback callback)
    : api_key_(std::move(api_key)),
      symbol_(std::move(symbol)),
      synthetic_spread_(synthetic_spread),
      callback_(std::move(callback)),
      ws_(std::make_unique<ix::WebSocket>()) {}

FinnhubClient::~FinnhubClient() { stop(); }

void FinnhubClient::start() {
    std::string url = "wss://ws.finnhub.io?token=" + api_key_;
    ws_->setUrl(url);

    ws_->setOnMessageCallback([this](const ix::WebSocketMessagePtr& msg) {
        if (msg->type == ix::WebSocketMessageType::Open) {
            if (ever_connected_.exchange(true)) {
                ++reconnect_count_;
            }
            connected_.store(true);
            json sub = {{"type", "subscribe"}, {"symbol", symbol_}};
            ws_->send(sub.dump());
            std::cerr << "[finnhub] connected, subscribed to " << symbol_ << "\n";
        } else if (msg->type == ix::WebSocketMessageType::Close) {
            connected_.store(false);
            std::cerr << "[finnhub] connection closed\n";
        } else if (msg->type == ix::WebSocketMessageType::Error) {
            connected_.store(false);
            std::cerr << "[finnhub] error: " << msg->errorInfo.reason << "\n";
        } else if (msg->type == ix::WebSocketMessageType::Message) {
            ++messages_received_;
            handle_text_message(msg->str);
        }
    });

    ix::WebSocketPerMessageDeflateOptions deflate_options;
    ws_->setPerMessageDeflateOptions(deflate_options);
    ws_->enableAutomaticReconnection();
    ws_->start();
}

void FinnhubClient::stop() {
    if (ws_) ws_->stop();
    connected_.store(false);
}

void FinnhubClient::handle_text_message(const std::string& payload) {
    json parsed;
    try {
        parsed = json::parse(payload);
    } catch (const json::parse_error&) {
        ++malformed_messages_;
        return;
    }

    auto type_it = parsed.find("type");
    if (type_it == parsed.end()) {
        ++malformed_messages_;
        return;
    }
    std::string type = type_it->get<std::string>();

    if (type == "ping") {
        return;
    }
    if (type != "trade") {
        return;
    }

    auto data_it = parsed.find("data");
    if (data_it == parsed.end() || !data_it->is_array()) {
        ++malformed_messages_;
        return;
    }

    for (const auto& tick : *data_it) {
        if (!tick.contains("p") || !tick.contains("v") || !tick.contains("t") ||
            !tick.contains("s")) {
            ++malformed_messages_;
            continue;
        }
        double price = tick["p"].get<double>();
        double volume = tick["v"].get<double>();
        int64_t ms = tick["t"].get<int64_t>();
        std::string sym = tick["s"].get<std::string>();

        Timestamp ts = std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds(ms));

        // Tick rule: classify aggressor side from the price change since the
        // last trade, since Finnhub's free trade tape does not label side.
        Side side = last_side_;
        if (last_trade_price_) {
            if (price > *last_trade_price_) side = Side::Buy;
            else if (price < *last_trade_price_) side = Side::Sell;
        }
        last_trade_price_ = price;
        last_side_ = side;

        SequenceNumber seq = next_sequence_.fetch_add(1);
        Symbol symbol_obj(sym);
        Price px = Price::from_double(price);
        Quantity qty = static_cast<Quantity>(volume);

        callback_(MarketEvent::make_trade(ts, seq, symbol_obj, side, px, qty));

        // Synthetic top-of-book overlay derived from the real trade print --
        // see the header comment for why this is necessary and how it is
        // clearly not real order-book depth.
        double half = synthetic_spread_ / 2.0;
        SequenceNumber bid_seq = next_sequence_.fetch_add(1);
        callback_(MarketEvent::make_quote(ts, bid_seq, symbol_obj, Side::Buy,
                                           Price::from_double(price - half), qty));
        SequenceNumber ask_seq = next_sequence_.fetch_add(1);
        callback_(MarketEvent::make_quote(ts, ask_seq, symbol_obj, Side::Sell,
                                           Price::from_double(price + half), qty));
    }
}

}  // namespace lob
