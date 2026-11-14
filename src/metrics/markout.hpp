#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <vector>

#include "common/types.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Post-fill "markout" analysis, the standard way to measure adverse selection
// for a market maker.
//
// For each fill we record the mid price at fill time and, for each horizon h,
// the mid price h of event-time later. With s = +1 for a buy and -1 for a sell:
//   edge          = s * (mid_at_fill - fill_price)      captured at the fill
//   markout(h)    = s * (mid_at(t+h) - fill_price)      what the fill was worth h later
//   adverse(h)    = edge - markout(h)                   mid moved against us
// All figures are per share, volume-weighted. Markout well below edge means
// fills tend to be followed by price moves against the maker.
//
// Time handling (deterministic, uses event timestamps only): call on_time(ts,
// mid) BEFORE applying the event at ts, passing the mid that was in force just
// before it; any fill whose t+h <= ts is then marked at that mid.
// ---------------------------------------------------------------------------
class MarkoutTracker {
public:
    static constexpr size_t kMaxHorizons = 6;

    explicit MarkoutTracker(std::vector<Timestamp> horizons = {}) {
        for (size_t i = 0; i < horizons.size() && i < kMaxHorizons; ++i) {
            horizons_[i] = horizons[i];
            ++horizon_count_;
        }
    }

    [[nodiscard]] size_t horizon_count() const noexcept { return horizon_count_; }
    [[nodiscard]] Timestamp horizon(size_t i) const noexcept { return horizons_[i]; }

    void on_fill(Side side, double fill_price, Quantity qty, double mid_at_fill, Timestamp now) {
        if (qty <= 0) return;
        const double sign = side == Side::Buy ? 1.0 : -1.0;
        const double q = static_cast<double>(qty);
        edge_sum_ += sign * (mid_at_fill - fill_price) * q;
        qty_sum_ += q;
        ++fill_count_;
        for (size_t h = 0; h < horizon_count_; ++h) {
            pending_[h].push_back({now + horizons_[h], sign, fill_price, q});
        }
    }

    void on_time(Timestamp ts, double mid_before_event) {
        for (size_t h = 0; h < horizon_count_; ++h) {
            auto& dq = pending_[h];
            while (!dq.empty() && dq.front().due <= ts) {
                const Pending& p = dq.front();
                markout_sum_[h] += p.sign * (mid_before_event - p.price) * p.qty;
                marked_qty_[h] += p.qty;
                dq.pop_front();
            }
        }
    }

    [[nodiscard]] size_t fill_count() const noexcept { return fill_count_; }
    [[nodiscard]] double filled_qty() const noexcept { return qty_sum_; }
    [[nodiscard]] double avg_edge() const noexcept { return qty_sum_ > 0 ? edge_sum_ / qty_sum_ : 0.0; }
    // Fills not yet old enough to be marked at horizon h are excluded.
    [[nodiscard]] double avg_markout(size_t h) const noexcept {
        return marked_qty_[h] > 0 ? markout_sum_[h] / marked_qty_[h] : 0.0;
    }
    [[nodiscard]] double marked_qty(size_t h) const noexcept { return marked_qty_[h]; }

private:
    struct Pending {
        Timestamp due;
        double sign;
        double price;
        double qty;
    };

    std::array<Timestamp, kMaxHorizons> horizons_{};
    size_t horizon_count_ = 0;
    std::array<std::deque<Pending>, kMaxHorizons> pending_;
    std::array<double, kMaxHorizons> markout_sum_{};
    std::array<double, kMaxHorizons> marked_qty_{};
    double edge_sum_ = 0.0;
    double qty_sum_ = 0.0;
    size_t fill_count_ = 0;
};

// Running portfolio-level analytics, O(1) per event.
struct RunAnalytics {
    Quantity max_abs_position = 0;
    double sum_abs_position = 0.0;  // averaged over events (event-weighted, not time-weighted)
    size_t samples = 0;
    double peak_pnl = 0.0;
    double max_drawdown = 0.0;      // largest peak-to-trough fall in total P&L, >= 0

    void sample(Quantity position, double total_pnl) {
        Quantity a = position < 0 ? -position : position;
        if (a > max_abs_position) max_abs_position = a;
        sum_abs_position += static_cast<double>(a);
        ++samples;
        if (total_pnl > peak_pnl) peak_pnl = total_pnl;
        double dd = peak_pnl - total_pnl;
        if (dd > max_drawdown) max_drawdown = dd;
    }
    [[nodiscard]] double mean_abs_position() const noexcept {
        return samples ? sum_abs_position / static_cast<double>(samples) : 0.0;
    }
};

}  // namespace lob
