#pragma once

#include <string>

#include "common/types.hpp"

namespace lob {

struct Order {
    OrderId id = 0;
    Symbol symbol;
    Side side = Side::Buy;
    OrderType type = OrderType::Limit;
    TimeInForce tif = TimeInForce::GTC;
    Price price{};              // ignored for OrderType::Market
    Quantity quantity = 0;      // original requested quantity
    Quantity filled_quantity = 0;
    OrderStatus status = OrderStatus::New;
    Timestamp created_time{};
    Timestamp last_update_time{};

    // Simulation state (see PaperExecutionEngine).
    Timestamp active_time{};         // orders cannot fill before this event time
    Timestamp cancel_effective{};    // valid when cancel_pending
    bool cancel_pending = false;
    double queue_ahead = 0.0;        // shares estimated to be ahead of this order at its price
    Quantity level_qty_seen = 0;     // last level quantity observed at this order's price
    Quantity trades_since_update = 0;

    [[nodiscard]] Quantity remaining_quantity() const noexcept {
        return quantity - filled_quantity;
    }
};

struct ExecutionReport {
    OrderId order_id = 0;
    Symbol symbol;
    Side side = Side::Buy;
    OrderStatus status = OrderStatus::New;
    Price fill_price{};
    Quantity fill_quantity = 0;      // quantity filled by *this* report, not cumulative
    Quantity remaining_quantity = 0;
    Timestamp timestamp{};
    Timestamp submit_to_ack_latency{};  // the configured simulated order latency
    double fee = 0.0;                   // fee charged on this fill (negative = rebate)
    bool taker = false;                 // true if this fill removed liquidity
    std::string reason;                 // populated on Rejected
};

}  // namespace lob
