#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>

#include "common/types.hpp"
#include "orderbook/order_book.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Market microstructure metrics computed from the live order-book state.
//
// Formulas:
//   mid_price      = (best_bid + best_ask) / 2
//   spread         = best_ask - best_bid
//   spread_bps     = spread / mid_price * 10,000
//   microprice     = (best_bid * ask_qty + best_ask * bid_qty)
//                     / (bid_qty + ask_qty)
//                    A liquidity-weighted price that leans toward the side
//                    with less resting size (the side more likely to move),
//                    which is a better short-horizon fair-value estimate
//                    than the simple mid.
//   order_book_imbalance (top of book) =
//                     (bid_qty - ask_qty) / (bid_qty + ask_qty)
//                    Ranges [-1, 1]; positive means more resting buy
//                    interest than sell interest at the top of book.
//   depth_imbalance (N levels) = same formula, summed over the top N
//                    levels on each side instead of just level 1.
//   realized volatility = stddev of log mid-price returns over a rolling
//                    window of the last N mid-price samples, annualization
//                    intentionally omitted -- this is used only as a
//                    relative short-term signal for the strategy, not as a
//                    calibrated volatility estimate.
// ---------------------------------------------------------------------------
class MicrostructureCalculator {
public:
    explicit MicrostructureCalculator(size_t volatility_window = 50,
                                       size_t trade_window = 200)
        : volatility_window_(volatility_window), trade_window_(trade_window) {}

    // Call once per order-book update to keep rolling windows current.
    void on_book_update(const OrderBook& book, Timestamp ts) {
        auto mid = book.mid_price();
        if (!mid) return;
        if (last_mid_) {
            double log_ret = std::log(*mid / *last_mid_);
            returns_.push_back(log_ret);
            if (returns_.size() > volatility_window_) returns_.pop_front();
        }
        last_mid_ = mid;
        (void)ts;
    }

    void on_trade(Quantity qty, Timestamp ts) {
        trades_.push_back({qty, ts});
        while (trades_.size() > trade_window_) trades_.pop_front();
    }

    [[nodiscard]] static std::optional<double> spread_bps(const OrderBook& book) noexcept {
        auto sp = book.spread();
        auto mid = book.mid_price();
        if (!sp || !mid || *mid == 0.0) return std::nullopt;
        return (*sp / *mid) * 10'000.0;
    }

    [[nodiscard]] static std::optional<double> microprice(const OrderBook& book) noexcept {
        auto bb = book.best_bid();
        auto ba = book.best_ask();
        auto bq = book.best_bid_quantity();
        auto aq = book.best_ask_quantity();
        if (!bb || !ba || !bq || !aq) return std::nullopt;
        double denom = static_cast<double>(*bq + *aq);
        if (denom == 0.0) return std::nullopt;
        return (bb->to_double() * static_cast<double>(*aq) +
                ba->to_double() * static_cast<double>(*bq)) / denom;
    }

    [[nodiscard]] static std::optional<double> top_of_book_imbalance(const OrderBook& book) noexcept {
        auto bq = book.best_bid_quantity();
        auto aq = book.best_ask_quantity();
        if (!bq || !aq) return std::nullopt;
        double denom = static_cast<double>(*bq + *aq);
        if (denom == 0.0) return std::nullopt;
        return static_cast<double>(*bq - *aq) / denom;
    }

    [[nodiscard]] static std::optional<double> depth_imbalance(const OrderBook& book, size_t levels) {
        auto bids = book.bid_depth(levels);
        auto asks = book.ask_depth(levels);
        if (bids.empty() || asks.empty()) return std::nullopt;
        Quantity bid_vol = 0, ask_vol = 0;
        for (auto& l : bids) bid_vol += l.quantity;
        for (auto& l : asks) ask_vol += l.quantity;
        double denom = static_cast<double>(bid_vol + ask_vol);
        if (denom == 0.0) return std::nullopt;
        return static_cast<double>(bid_vol - ask_vol) / denom;
    }

    [[nodiscard]] std::optional<double> short_term_volatility() const noexcept {
        if (returns_.size() < 2) return std::nullopt;
        double mean = 0.0;
        for (double r : returns_) mean += r;
        mean /= static_cast<double>(returns_.size());
        double var = 0.0;
        for (double r : returns_) var += (r - mean) * (r - mean);
        var /= static_cast<double>(returns_.size() - 1);
        return std::sqrt(var);
    }

    [[nodiscard]] Quantity rolling_trade_volume() const noexcept {
        Quantity total = 0;
        for (auto& t : trades_) total += t.quantity;
        return total;
    }

    // Trades per second over the rolling trade window.
    [[nodiscard]] std::optional<double> trade_intensity() const noexcept {
        if (trades_.size() < 2) return std::nullopt;
        auto dt_ns = (trades_.back().timestamp - trades_.front().timestamp).count();
        if (dt_ns <= 0) return std::nullopt;
        double dt_sec = static_cast<double>(dt_ns) / 1e9;
        return static_cast<double>(trades_.size()) / dt_sec;
    }

private:
    struct TradeSample {
        Quantity quantity;
        Timestamp timestamp;
    };

    size_t volatility_window_;
    size_t trade_window_;
    std::optional<double> last_mid_;
    std::deque<double> returns_;
    std::deque<TradeSample> trades_;
};

}  // namespace lob
