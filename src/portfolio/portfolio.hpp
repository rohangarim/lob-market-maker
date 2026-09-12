#pragma once

#include <algorithm>
#include <cmath>
#include <vector>

#include "execution/order.hpp"

namespace lob {

struct PositionState {
    Quantity position = 0;          // signed: +long / -short
    double avg_entry_price = 0.0;
    double realized_pnl = 0.0;
    double unrealized_pnl = 0.0;
    double total_pnl = 0.0;
    Quantity total_volume_traded = 0;  // sum of |fill quantities|
    double turnover = 0.0;             // sum of |fill quantity * fill price|
};

// ---------------------------------------------------------------------------
// Tracks position, average entry price, and realized/unrealized/total P&L
// for a single symbol from a stream of execution fills plus mark price
// updates. Uses standard weighted-average-cost accounting:
//
//  - A fill on the same side as the current position (or opening a flat
//    position) extends the position and updates avg_entry_price as the
//    quantity-weighted average of the old and new cost basis.
//  - A fill on the opposite side realizes P&L on the portion that closes
//    existing position: realized_pnl += closed_qty * (fill_price -
//    avg_entry_price) * position_sign. If the fill size exceeds the open
//    position, the remainder flips the position to the other side at the
//    fill price as the new avg_entry_price.
//  - unrealized_pnl is recomputed on every mark-price update as
//    position * (mark_price - avg_entry_price) * position_sign, i.e.
//    mark-to-market against the current mid-price (or a configurable mark).
//  - total_pnl = realized_pnl + unrealized_pnl.
//
// Single-owner, not thread-safe -- intended to be driven exclusively by the
// order-book/strategy thread that also owns the execution engine.
// ---------------------------------------------------------------------------
class Portfolio {
public:
    void on_fill(Side side, Price fill_price, Quantity fill_qty) {
        double price = fill_price.to_double();
        double qty = static_cast<double>(fill_qty);
        double signed_fill = (side == Side::Buy) ? qty : -qty;

        state_.total_volume_traded += fill_qty;
        state_.turnover += qty * price;

        if (state_.position == 0 ||
            (state_.position > 0) == (signed_fill > 0)) {
            // Extending (or opening) a position in the same direction.
            double old_notional = static_cast<double>(state_.position) * state_.avg_entry_price;
            double new_position = static_cast<double>(state_.position) + signed_fill;
            double new_notional = old_notional + signed_fill * price;
            state_.avg_entry_price = (new_position != 0.0) ? new_notional / new_position : 0.0;
            state_.position = static_cast<Quantity>(new_position);
        } else {
            // Reducing or flipping the position.
            double closing_qty = std::min(std::abs(signed_fill), std::abs(static_cast<double>(state_.position)));
            int position_sign = (state_.position > 0) ? 1 : -1;
            state_.realized_pnl += closing_qty * (price - state_.avg_entry_price) * position_sign;

            double new_position = static_cast<double>(state_.position) + signed_fill;
            state_.position = static_cast<Quantity>(new_position);
            if ((state_.position > 0) != (position_sign > 0) && state_.position != 0) {
                // Flipped sides: the excess quantity opens a fresh position
                // at this fill's price.
                state_.avg_entry_price = price;
            } else if (state_.position == 0) {
                state_.avg_entry_price = 0.0;
            }
        }
        record_history();
    }

    void mark_to_market(double mark_price) {
        // position is signed (+long/-short), so this single expression is
        // correct for both: a short (negative position) profits when
        // mark_price falls below avg_entry_price, which the sign handles.
        state_.unrealized_pnl = static_cast<double>(state_.position) * (mark_price - state_.avg_entry_price);
        state_.total_pnl = state_.realized_pnl + state_.unrealized_pnl;
    }

    [[nodiscard]] const PositionState& state() const noexcept { return state_; }

    [[nodiscard]] const std::vector<PositionState>& history() const noexcept { return history_; }

private:
    void record_history() {
        history_.push_back(state_);
    }

    PositionState state_;
    std::vector<PositionState> history_;
};

}  // namespace lob
