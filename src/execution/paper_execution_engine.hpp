#pragma once

#include <optional>
#include <random>
#include <vector>

#include "execution/order.hpp"
#include "orderbook/order_book.hpp"

namespace lob {

enum class FillModel : uint8_t {
    // Optimistic baseline: a resting limit fills whenever a trade prints at or
    // through its price, with randomized partial fills capped by the trade size.
    TradeThrough,
    // Queue-position model (default). See FillAssumptions.
    QueuePosition,
};

// ---------------------------------------------------------------------------
// Fill assumptions for the paper execution simulator. All of it is a model,
// not an exchange: documented here so results can be interpreted honestly.
//
// QueuePosition model (default)
//   - On submission the order joins the BACK of the queue at its price. The
//     shares ahead of it are the book's resting quantity at that price at
//     submission time (0 if the price is empty or better than the touch).
//   - A trade printing at exactly the order's price consumes the queue ahead
//     first; only the excess fills the order. A trade printing strictly
//     THROUGH the price (below a buy limit / above a sell limit) implies the
//     whole level was exhausted, so the order's remaining size fills.
//   - Level-size changes at the order's price: shares explained by trades
//     since the last update are not double counted; any further decrease is
//     treated as cancellations spread uniformly over the queue, so the shares
//     ahead shrink pro rata. Increases join behind the order and change
//     nothing. A deleted level means nothing is ahead.
//   - Fills are fully determined by the event stream (no randomness).
//   - The aggressor side of a print is ignored (prints are matched by price
//     only), because inferred sides on a trade-only feed are unreliable.
// TradeThrough model: see FillModel; uses partial_fill_probability and the
//   partial-fill ratio range, drawn from a seeded RNG.
// Both models
//   - A limit order that is marketable on arrival (buy at/above the best ask,
//     sell at/below the best bid) first takes liquidity from the visible book,
//     level by level, as a taker; any remainder rests.
//   - Market orders sweep the opposite side's visible depth; the unfilled
//     remainder is dropped (market orders never rest).
//   - Limit fills at the order's own price (no price improvement).
//   - Latencies are measured in EVENT time (feed timestamps): an order cannot
//     fill before submit + order_latency, and a cancel takes effect only at
//     request + cancel_latency, so fills can still occur in that window.
//   - Fees are per share; a negative maker fee is a rebate.
//   - Not modelled: hidden/iceberg liquidity, our own market impact on the
//     displayed book (our orders never appear in it), self-trade, order
//     priority loss on modify, exchange-specific matching rules.
// ---------------------------------------------------------------------------
struct FillAssumptions {
    FillModel fill_model = FillModel::QueuePosition;
    double partial_fill_probability = 0.3;  // TradeThrough only
    double min_partial_fill_ratio = 0.2;    // TradeThrough only
    double max_partial_fill_ratio = 0.9;    // TradeThrough only
    Timestamp order_latency{0};
    Timestamp cancel_latency{0};
    double maker_fee_per_share = 0.0;
    double taker_fee_per_share = 0.0;
    uint64_t rng_seed = 42;
};

struct SubmitResult {
    ExecutionReport ack;
    std::optional<ExecutionReport> taker_fill;  // immediate fill of a marketable order
};

class PaperExecutionEngine {
public:
    explicit PaperExecutionEngine(FillAssumptions assumptions = {})
        : assumptions_(assumptions), rng_(assumptions.rng_seed) {}

    // Risk checks happen *before* these; the engine assumes approval.

    // Rests a limit order with nothing ahead of it (no book knowledge).
    ExecutionReport submit_limit(OrderId id, Symbol symbol, Side side, Price price,
                                  Quantity quantity, Timestamp now);

    // Book-aware: takes liquidity first if marketable, then rests the rest
    // behind the visible quantity at its price.
    SubmitResult submit_limit(OrderId id, Symbol symbol, Side side, Price price,
                               Quantity quantity, Timestamp now, const OrderBook& book);

    ExecutionReport submit_market(OrderId id, Symbol symbol, Side side, Quantity quantity,
                                   const OrderBook& book, Timestamp now);

    // With cancel_latency == 0 the order is removed immediately and the report
    // returned. Otherwise the cancel is queued and the Canceled report arrives
    // from on_market_event() once its effective time is reached (nullopt here).
    std::optional<ExecutionReport> cancel(OrderId id, Timestamp now);

    // Feed every market event here after it has been applied to the book.
    std::vector<ExecutionReport> on_market_event(const MarketEvent& ev);

    [[nodiscard]] size_t open_order_count() const noexcept { return open_orders_.size(); }
    [[nodiscard]] std::optional<Order> get_order(OrderId id) const;
    [[nodiscard]] const std::vector<Order>& open_orders() const noexcept { return open_orders_; }

    [[nodiscard]] size_t total_submitted() const noexcept { return total_submitted_; }
    [[nodiscard]] size_t total_canceled() const noexcept { return total_canceled_; }
    [[nodiscard]] size_t total_filled() const noexcept { return total_filled_; }
    [[nodiscard]] size_t total_partial_fills() const noexcept { return total_partial_fills_; }
    [[nodiscard]] size_t total_taker_fills() const noexcept { return total_taker_fills_; }

private:
    ExecutionReport make_fill_report(Order& order, Price fill_price, Quantity fill_qty,
                                      Timestamp now, bool taker);
    ExecutionReport make_cancel_report(const Order& order, Timestamp now) const;
    Quantity eligible_fill_queue_model(Order& order, const MarketEvent& ev);
    Quantity eligible_fill_trade_through(const Order& order, const MarketEvent& ev);
    static void update_level(Order& order, Quantity new_level_qty);
    Order* find(OrderId id);

    FillAssumptions assumptions_;
    std::mt19937_64 rng_;
    std::vector<Order> open_orders_;  // insertion (= id) order, so iteration is deterministic

    size_t total_submitted_ = 0;
    size_t total_canceled_ = 0;
    size_t total_filled_ = 0;
    size_t total_partial_fills_ = 0;
    size_t total_taker_fills_ = 0;
};

}  // namespace lob
