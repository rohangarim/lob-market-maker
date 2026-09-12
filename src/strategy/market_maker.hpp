#pragma once

#include <algorithm>

#include "common/types.hpp"
#include "metrics/microstructure.hpp"
#include "orderbook/order_book.hpp"

namespace lob {

struct StrategyConfig {
    double base_half_spread_bps = 5.0;
    Quantity quote_size = 10;
    // Basis points the mid reference price shifts per unit of inventory,
    // clamped at max_inventory_for_skew units of skew.
    double inventory_skew_bps_per_unit = 0.5;
    Quantity max_inventory_for_skew = 200;
    // Multiplies the base half-spread by (1 + multiplier * normalized_vol).
    double volatility_spread_multiplier = 1.5;
    // Stop quoting if the book hasn't updated in this many seconds
    // (guards against quoting on stale/disconnected market data).
    double max_staleness_sec = 5.0;
};

struct QuoteDecision {
    bool quote_bid = false;
    bool quote_ask = false;
    Price bid_price{};
    Price ask_price{};
    Quantity bid_size = 0;
    Quantity ask_size = 0;
};

// ---------------------------------------------------------------------------
// A research-oriented market-making strategy. This is NOT a predictive
// alpha model -- it quotes symmetrically around a microstructure-informed
// reference price, widens in response to short-term volatility, and skews
// its quotes to manage inventory risk. Every parameter below is
// configurable (see config/config.example.json).
//
// Reference price and spread:
//   half_spread_bps = base_half_spread_bps * (1 + volatility_spread_multiplier
//                                              * normalized_short_term_vol)
//   where normalized_short_term_vol scales the microstructure module's
//   stddev-of-log-returns into a comparable bps-like magnitude.
//
// Inventory skew (see README "Strategy" section for the worked example):
//   inv_skew_dollars = inventory_skew_bps_per_unit/10000
//                       * clamp(inventory, -max_inventory_for_skew, +max)
//                       * mid_price
//   bid_price = mid - half_spread_dollars - inv_skew_dollars
//   ask_price = mid + half_spread_dollars - inv_skew_dollars
//   A positive (long) inventory shifts inv_skew_dollars positive, which
//   pulls BOTH quotes down: the bid becomes less competitive (discourages
//   buying more) and the ask becomes more competitive (encourages
//   selling down the excess long). A negative (short) inventory does the
//   mirror image.
//
// Quote size tapering: as inventory approaches max_inventory_for_skew in
// either direction, the size on the side that would extend the position
// further is tapered toward zero (and that side stops quoting entirely at
// the limit), while the side that would reduce the position keeps full
// size to encourage flattening.
// ---------------------------------------------------------------------------
class MarketMakerStrategy {
public:
    explicit MarketMakerStrategy(StrategyConfig cfg) : cfg_(cfg) {}

    [[nodiscard]] QuoteDecision decide(const OrderBook& book,
                                        const MicrostructureCalculator& micro,
                                        Quantity current_inventory,
                                        Timestamp now) const {
        QuoteDecision decision;

        auto mid_opt = book.mid_price();
        if (!mid_opt) return decision;  // no two-sided market, cannot quote

        double staleness_sec =
            static_cast<double>((now - book.last_update_time()).count()) / 1e9;
        if (staleness_sec > cfg_.max_staleness_sec) {
            return decision;  // stale market data: stop generating new quotes
        }

        double mid = *mid_opt;
        double vol = micro.short_term_volatility().value_or(0.0);
        double normalized_vol = vol * 10'000.0;  // scale log-return stddev to a bps-like magnitude
        double half_spread_bps =
            cfg_.base_half_spread_bps * (1.0 + cfg_.volatility_spread_multiplier * normalized_vol);
        double half_spread_dollars = mid * half_spread_bps / 10'000.0;

        double clamped_inventory = std::clamp(static_cast<double>(current_inventory),
                                               -static_cast<double>(cfg_.max_inventory_for_skew),
                                               static_cast<double>(cfg_.max_inventory_for_skew));
        double inv_skew_dollars =
            (cfg_.inventory_skew_bps_per_unit / 10'000.0) * clamped_inventory * mid;

        double bid = mid - half_spread_dollars - inv_skew_dollars;
        double ask = mid + half_spread_dollars - inv_skew_dollars;
        if (bid <= 0.0 || ask <= bid) return decision;  // degenerate quotes, refuse to trade

        double inv_ratio = (cfg_.max_inventory_for_skew > 0)
                                ? clamped_inventory / static_cast<double>(cfg_.max_inventory_for_skew)
                                : 0.0;
        double bid_taper = (inv_ratio > 0.0) ? std::max(0.0, 1.0 - inv_ratio) : 1.0;
        double ask_taper = (inv_ratio < 0.0) ? std::max(0.0, 1.0 + inv_ratio) : 1.0;

        Quantity bid_size = static_cast<Quantity>(static_cast<double>(cfg_.quote_size) * bid_taper);
        Quantity ask_size = static_cast<Quantity>(static_cast<double>(cfg_.quote_size) * ask_taper);

        decision.bid_price = Price::from_double(bid);
        decision.ask_price = Price::from_double(ask);
        decision.bid_size = bid_size;
        decision.ask_size = ask_size;
        decision.quote_bid = bid_size > 0;
        decision.quote_ask = ask_size > 0;
        return decision;
    }

private:
    StrategyConfig cfg_;
};

}  // namespace lob
