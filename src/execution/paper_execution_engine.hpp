#pragma once

#include <optional>
#include <random>
#include <unordered_map>
#include <vector>

#include "execution/order.hpp"
#include "orderbook/order_book.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Configurable fill assumptions for the paper execution simulator.
//
// Documented assumptions (see README section "Execution simulator"):
//  - Market orders walk the *current* resting book depth on the opposite
//    side and fill against it level by level at each level's resting
//    price (a standard "sweep the book" simulation). Any quantity beyond
//    available depth is left unfilled and the order is closed (market
//    orders never rest).
//  - Limit orders rest in the book until a subsequent Trade event prints
//    at a price that crosses the resting limit (trade price <= limit for
//    a resting buy, >= limit for a resting sell). This models "the tape
//    traded through my price" but does NOT model queue position ahead of
//    the order at that price level, nor iceberg/hidden liquidity ahead of
//    it -- a real order at the back of a deep queue would fill later or
//    not at all. This is a simplification appropriate for a research/paper
//    system, not a queue-accurate exchange simulator.
//  - When a crossing trade is observed, the fill is partial with
//    probability partial_fill_probability, and full otherwise. Partial
//    fill size is a uniformly random fraction of the remaining quantity
//    in [min_partial_fill_ratio, max_partial_fill_ratio]. Fill quantity
//    is also capped by the crossing trade's own printed quantity, since a
//    resting order cannot fill for more shares than actually traded.
//  - Fill price for limit orders is always the resting limit price itself
//    (no price improvement is modeled).
//  - simulated_ack_latency models the round-trip from order submission to
//    exchange acknowledgement; it is added to the reported execution
//    latency but does not delay when a fill can logically occur.
//  - All randomness is drawn from an RNG seeded explicitly at construction,
//    so replay runs are bit-for-bit deterministic given the same seed and
//    input event stream.
// ---------------------------------------------------------------------------
struct FillAssumptions {
    double partial_fill_probability = 0.3;
    double min_partial_fill_ratio = 0.2;
    double max_partial_fill_ratio = 0.9;
    Timestamp simulated_ack_latency = std::chrono::microseconds(50);
    uint64_t rng_seed = 42;
};

class PaperExecutionEngine {
public:
    explicit PaperExecutionEngine(FillAssumptions assumptions = {})
        : assumptions_(assumptions), rng_(assumptions.rng_seed) {}

    // Risk checks happen *before* calling these; the execution engine
    // assumes the order has already been approved.
    ExecutionReport submit_limit(OrderId id, Symbol symbol, Side side, Price price,
                                  Quantity quantity, Timestamp now);

    ExecutionReport submit_market(OrderId id, Symbol symbol, Side side, Quantity quantity,
                                   const OrderBook& book, Timestamp now);

    std::optional<ExecutionReport> cancel(OrderId id, Timestamp now);

    // Feed every market event through here after it has been applied to the
    // order book, so resting limit orders can be checked for fills.
    std::vector<ExecutionReport> on_market_event(const MarketEvent& ev);

    [[nodiscard]] size_t open_order_count() const noexcept { return open_orders_.size(); }
    [[nodiscard]] std::optional<Order> get_order(OrderId id) const;
    [[nodiscard]] std::vector<Order> open_orders() const;

    [[nodiscard]] size_t total_submitted() const noexcept { return total_submitted_; }
    [[nodiscard]] size_t total_canceled() const noexcept { return total_canceled_; }
    [[nodiscard]] size_t total_filled() const noexcept { return total_filled_; }
    [[nodiscard]] size_t total_partial_fills() const noexcept { return total_partial_fills_; }

private:
    ExecutionReport make_fill_report(Order& order, Price fill_price, Quantity fill_qty,
                                      Timestamp now);

    FillAssumptions assumptions_;
    std::mt19937_64 rng_;
    std::unordered_map<OrderId, Order> open_orders_;

    size_t total_submitted_ = 0;
    size_t total_canceled_ = 0;
    size_t total_filled_ = 0;
    size_t total_partial_fills_ = 0;
};

}  // namespace lob
