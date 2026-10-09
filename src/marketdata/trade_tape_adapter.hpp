#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

#include "marketdata/events.hpp"
#include "marketdata/finnhub_parser.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Turns a trade-only feed into the MarketEvent stream the pipeline expects.
//
// A trade tape carries no book depth, so for every real trade this adapter
// emits:
//   1. the real Trade event, with the aggressor side inferred by the tick rule
//      (uptick = Buy, downtick = Sell, unchanged = previous side);
//   2. a SYNTHETIC one-level book around the trade price (price -/+ half the
//      configured spread). When the synthetic levels move, the previous ones
//      are deleted FIRST, so exactly one synthetic bid and one synthetic ask
//      exist at any time and the book can never be left crossed by the overlay.
// Sequence numbers are assigned locally and are consecutive.
//
// Not thread-safe: use from the single thread that receives the feed.
// ---------------------------------------------------------------------------
class TradeTapeAdapter {
public:
    explicit TradeTapeAdapter(double synthetic_spread) : half_spread_(synthetic_spread / 2.0) {}

    void on_trade(const FinnhubTrade& trade, std::vector<MarketEvent>& out) {
        Symbol symbol(trade.symbol);
        Timestamp ts = std::chrono::duration_cast<Timestamp>(std::chrono::milliseconds(trade.timestamp_ms));
        Quantity qty = std::max<Quantity>(1, static_cast<Quantity>(std::llround(trade.volume)));

        Side side = last_side_;
        if (last_price_) {
            if (trade.price > *last_price_) side = Side::Buy;
            else if (trade.price < *last_price_) side = Side::Sell;
        }
        last_price_ = trade.price;
        last_side_ = side;

        out.push_back(MarketEvent::make_trade(ts, seq_++, symbol, side, Price::from_double(trade.price), qty));

        Price new_bid = Price::from_double(trade.price - half_spread_);
        Price new_ask = Price::from_double(trade.price + half_spread_);
        if (bid_ && *bid_ != new_bid) out.push_back(MarketEvent::make_delete(ts, seq_++, symbol, Side::Buy, *bid_));
        if (ask_ && *ask_ != new_ask) out.push_back(MarketEvent::make_delete(ts, seq_++, symbol, Side::Sell, *ask_));
        out.push_back(MarketEvent::make_quote(ts, seq_++, symbol, Side::Buy, new_bid, qty));
        out.push_back(MarketEvent::make_quote(ts, seq_++, symbol, Side::Sell, new_ask, qty));
        bid_ = new_bid;
        ask_ = new_ask;
    }

private:
    double half_spread_;
    SequenceNumber seq_ = 1;
    std::optional<double> last_price_;
    Side last_side_ = Side::Buy;
    std::optional<Price> bid_;
    std::optional<Price> ask_;
};

}  // namespace lob
