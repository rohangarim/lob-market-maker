#pragma once

#include <string>

#include "execution/order.hpp"

namespace lob {

struct RiskLimits {
    Quantity max_position = 1000;
    Quantity max_order_size = 100;
    double max_notional = 100'000.0;
    double max_daily_loss = 2'000.0;
    size_t max_open_orders = 20;
};

enum class RiskDecision : uint8_t {
    Approved = 0,
    RejectedMaxOrderSize,
    RejectedMaxPosition,
    RejectedMaxNotional,
    RejectedMaxDailyLoss,
    RejectedMaxOpenOrders,
    RejectedStaleMarketData,
};

inline std::string_view to_string(RiskDecision d) {
    switch (d) {
        case RiskDecision::Approved: return "APPROVED";
        case RiskDecision::RejectedMaxOrderSize: return "REJECTED_MAX_ORDER_SIZE";
        case RiskDecision::RejectedMaxPosition: return "REJECTED_MAX_POSITION";
        case RiskDecision::RejectedMaxNotional: return "REJECTED_MAX_NOTIONAL";
        case RiskDecision::RejectedMaxDailyLoss: return "REJECTED_MAX_DAILY_LOSS";
        case RiskDecision::RejectedMaxOpenOrders: return "REJECTED_MAX_OPEN_ORDERS";
        case RiskDecision::RejectedStaleMarketData: return "REJECTED_STALE_MARKET_DATA";
    }
    return "UNKNOWN";
}

struct RiskCheckInput {
    Quantity order_quantity = 0;
    Side order_side = Side::Buy;
    double reference_price = 0.0;   // mid or limit price used for notional calc
    Quantity current_position = 0;  // signed: +long / -short
    double current_daily_pnl = 0.0;
    size_t current_open_orders = 0;
    bool market_data_stale = false;
};

// ---------------------------------------------------------------------------
// Pure, stateless, deterministic pre-trade risk checks. Every order must be
// approved here before it reaches the paper execution engine. All checks
// are simple arithmetic/comparisons -- O(1), no allocation, no I/O -- so
// this stays off the critical-path-latency budget even at high order rates.
// ---------------------------------------------------------------------------
class RiskEngine {
public:
    explicit RiskEngine(RiskLimits limits) : limits_(limits) {}

    [[nodiscard]] RiskDecision check(const RiskCheckInput& in) const noexcept {
        if (in.market_data_stale) {
            return RiskDecision::RejectedStaleMarketData;
        }
        if (in.order_quantity <= 0 || in.order_quantity > limits_.max_order_size) {
            return RiskDecision::RejectedMaxOrderSize;
        }
        if (in.current_open_orders >= limits_.max_open_orders) {
            return RiskDecision::RejectedMaxOpenOrders;
        }
        if (in.current_daily_pnl <= -limits_.max_daily_loss) {
            return RiskDecision::RejectedMaxDailyLoss;
        }
        double notional = static_cast<double>(in.order_quantity) * in.reference_price;
        if (notional > limits_.max_notional) {
            return RiskDecision::RejectedMaxNotional;
        }
        Quantity signed_delta = (in.order_side == Side::Buy) ? in.order_quantity : -in.order_quantity;
        Quantity resulting_position = in.current_position + signed_delta;
        if (resulting_position > limits_.max_position || resulting_position < -limits_.max_position) {
            return RiskDecision::RejectedMaxPosition;
        }
        return RiskDecision::Approved;
    }

    [[nodiscard]] const RiskLimits& limits() const noexcept { return limits_; }

private:
    RiskLimits limits_;
};

}  // namespace lob
