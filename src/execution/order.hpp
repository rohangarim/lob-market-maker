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
    Timestamp submit_to_ack_latency{};  // simulated/measured execution latency
    std::string reason;                 // populated on Rejected
};

}  // namespace lob
