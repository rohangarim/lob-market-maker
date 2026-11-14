#pragma once

#include <algorithm>
#include <cmath>

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
    // A resting quote is left in place unless the new target price differs by
    // more than this many basis points of mid (or the size changed). Avoids
    // canceling and replacing on every sub-tick wobble of the target. Applied
    // by the Pipeline, which owns the resting orders. 0 disables hysteresis.
    double requote_threshold_bps = 1.0;
    // Fair-value adjustments (both off by default, so behaviour is unchanged):
    //   use_microprice: quote around the size-weighted microprice instead of mid.
    //   imbalance_skew_bps: shift the reference by imbalance_skew_bps * OBI bps
    //     of mid, where OBI is the bid/ask volume imbalance over the top
    //     imbalance_levels levels (+1 = all bids). Positive values lean quotes
    //     in the direction of the heavier side, which is where a persistent
    //     imbalance has historically been argued to predict the next move.
    //     Whether that holds is an empirical question for real data.
    // Tick handling (set tick_size = 0 to disable both):
    //   Quotes are rounded AWAY from the mid onto the tick grid (bid down, ask up),
    //   as real equity quotes must be, so they can coincide with book levels.
    //   post_only additionally clamps the bid to at most best_ask - tick and the
    //   ask to at least best_bid + tick, so a strong inventory skew can never make
    //   a quote cross the spread and turn the maker into a taker.
    double tick_size = 0.01;
    bool post_only = true;
    bool use_microprice = false;
    double imbalance_skew_bps = 0.0;
    int imbalance_levels = 3;
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
        double ref = mid;
        if (cfg_.use_microprice) {
            if (auto mp = MicrostructureCalculator::microprice(book)) ref = *mp;
        }
        if (cfg_.imbalance_skew_bps != 0.0) {
            if (auto oi = MicrostructureCalculator::depth_imbalance(
                    book, static_cast<size_t>(std::max(1, cfg_.imbalance_levels)))) {
                ref += (cfg_.imbalance_skew_bps / 10'000.0) * (*oi) * mid;
            }
        }
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

        double bid = ref - half_spread_dollars - inv_skew_dollars;
        double ask = ref + half_spread_dollars - inv_skew_dollars;
        if (cfg_.tick_size > 0.0) {
            const double tick = cfg_.tick_size;
            if (cfg_.post_only) {
                bid = std::min(bid, book.best_ask()->to_double() - tick);
                ask = std::max(ask, book.best_bid()->to_double() + tick);
            }
            // The epsilon keeps a price that is already on the grid from rounding a tick away.
            bid = std::floor(bid / tick + 1e-9) * tick;
            ask = std::ceil(ask / tick - 1e-9) * tick;
        }
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
