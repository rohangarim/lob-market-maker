#include "marketdata/finnhub_client.hpp"

#include <ixwebsocket/IXWebSocket.h>
#include <nlohmann/json.hpp>

#include <iostream>

namespace lob {

using json = nlohmann::json;  // used for the subscribe message

FinnhubClient::FinnhubClient(std::string api_key, std::string symbol, double synthetic_spread,
                              EventCallback callback)
    : api_key_(std::move(api_key)),
      symbol_(std::move(symbol)),
      callback_(std::move(callback)),
      ws_(std::make_unique<ix::WebSocket>()),
      adapter_(synthetic_spread) {}

FinnhubClient::~FinnhubClient() { stop(); }

void FinnhubClient::start() {
    std::string url = "wss://ws.finnhub.io/?token=" + api_key_;
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
    FinnhubParseResult parsed = parse_finnhub_message(payload);
    if (parsed.status == FinnhubParseStatus::Malformed) {
        ++malformed_messages_;
        return;
    }
    malformed_messages_ += parsed.malformed_ticks;

    batch_.clear();
    for (const auto& trade : parsed.trades) adapter_.on_trade(trade, batch_);
    for (const auto& ev : batch_) callback_(ev);
}

}  // namespace lob
