#pragma once

#include <algorithm>
#include <cmath>
#include <deque>
#include <optional>
#include <vector>

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
        if (last_mid_) push_return(std::log(*mid / *last_mid_));
        last_mid_ = mid;
        (void)ts;
    }

    // Forget the price history (used after a feed resync so the jump across the
    // gap is not counted as a volatility sample).
    void reset_price_history() {
        last_mid_.reset();
        ret_count_ = 0;
        ret_head_ = 0;
        ret_sum_ = ret_sumsq_ = 0.0;
        pushes_since_recompute_ = 0;
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

    // Sample standard deviation (n-1) of the last `volatility_window` log returns,
    // O(1) per call from running sums (see push_return for the drift control).
    [[nodiscard]] std::optional<double> short_term_volatility() const noexcept {
        if (ret_count_ < 2) return std::nullopt;
        const double n = static_cast<double>(ret_count_);
        const double var = (ret_sumsq_ - ret_sum_ * ret_sum_ / n) / (n - 1.0);
        return std::sqrt(var > 0.0 ? var : 0.0);
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
    // Running sum / sum of squares over a fixed ring. Subtracting evicted values
    // accumulates floating-point error, so every kRecomputeEvery pushes both sums
    // are rebuilt exactly from the ring contents (cost amortized to ~nothing).
    void push_return(double r) {
        if (ret_ring_.size() != volatility_window_) ret_ring_.assign(volatility_window_, 0.0);
        if (ret_count_ == volatility_window_) {
            const double old = ret_ring_[ret_head_];
            ret_sum_ -= old;
            ret_sumsq_ -= old * old;
        } else {
            ++ret_count_;
        }
        ret_ring_[ret_head_] = r;
        ret_sum_ += r;
        ret_sumsq_ += r * r;
        ret_head_ = (ret_head_ + 1) % volatility_window_;
        if (++pushes_since_recompute_ >= kRecomputeEvery) {
            pushes_since_recompute_ = 0;
            ret_sum_ = ret_sumsq_ = 0.0;
            for (size_t i = 0; i < ret_count_; ++i) {
                ret_sum_ += ret_ring_[i];
                ret_sumsq_ += ret_ring_[i] * ret_ring_[i];
            }
        }
    }

    static constexpr size_t kRecomputeEvery = 4096;

    struct TradeSample {
        Quantity quantity;
        Timestamp timestamp;
    };

    size_t volatility_window_;
    size_t trade_window_;
    std::optional<double> last_mid_;
    std::vector<double> ret_ring_;
    size_t ret_count_ = 0;
    size_t ret_head_ = 0;
    double ret_sum_ = 0.0;
    double ret_sumsq_ = 0.0;
    size_t pushes_since_recompute_ = 0;
    std::deque<TradeSample> trades_;
};

}  // namespace lob
