#include "execution/paper_execution_engine.hpp"

namespace lob {

ExecutionReport PaperExecutionEngine::submit_limit(OrderId id, Symbol symbol, Side side,
                                                    Price price, Quantity quantity,
                                                    Timestamp now) {
    ++total_submitted_;
    Order order;
    order.id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Limit;
    order.price = price;
    order.quantity = quantity;
    order.status = OrderStatus::New;
    order.created_time = now;
    order.last_update_time = now;
    open_orders_[id] = order;

    ExecutionReport report;
    report.order_id = id;
    report.symbol = symbol;
    report.side = side;
    report.status = OrderStatus::New;
    report.fill_price = price;
    report.fill_quantity = 0;
    report.remaining_quantity = quantity;
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.simulated_ack_latency;
    return report;
}

ExecutionReport PaperExecutionEngine::submit_market(OrderId id, Symbol symbol, Side side,
                                                     Quantity quantity, const OrderBook& book,
                                                     Timestamp now) {
    ++total_submitted_;
    Order order;
    order.id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Market;
    order.quantity = quantity;
    order.status = OrderStatus::New;
    order.created_time = now;
    order.last_update_time = now;

    // Sweep the opposite side's resting depth level by level.
    Quantity remaining = quantity;
    Quantity filled = 0;
    double notional = 0.0;
    size_t levels_needed = 64;
    auto opposite_depth = (side == Side::Buy) ? book.ask_depth(levels_needed)
                                               : book.bid_depth(levels_needed);
    for (const auto& level : opposite_depth) {
        if (remaining <= 0) break;
        Quantity take = std::min(remaining, level.quantity);
        filled += take;
        notional += take * level.price.to_double();
        remaining -= take;
    }

    order.filled_quantity = filled;
    order.status = (filled == quantity) ? OrderStatus::Filled
                    : (filled > 0)       ? OrderStatus::PartiallyFilled
                                         : OrderStatus::Rejected;

    ExecutionReport report;
    report.order_id = id;
    report.symbol = symbol;
    report.side = side;
    report.status = order.status;
    report.fill_price = (filled > 0) ? Price::from_double(notional / static_cast<double>(filled))
                                      : Price{};
    report.fill_quantity = filled;
    report.remaining_quantity = 0;  // market orders never rest
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.simulated_ack_latency;
    if (filled == 0) {
        report.reason = "no opposite-side liquidity available";
    }

    if (filled > 0) {
        ++total_filled_;
        if (filled < quantity) ++total_partial_fills_;
    }
    return report;
}

std::optional<ExecutionReport> PaperExecutionEngine::cancel(OrderId id, Timestamp now) {
    auto it = open_orders_.find(id);
    if (it == open_orders_.end()) return std::nullopt;

    ExecutionReport report;
    report.order_id = id;
    report.symbol = it->second.symbol;
    report.side = it->second.side;
    report.status = OrderStatus::Canceled;
    report.fill_quantity = 0;
    report.remaining_quantity = it->second.remaining_quantity();
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.simulated_ack_latency;

    open_orders_.erase(it);
    ++total_canceled_;
    return report;
}

ExecutionReport PaperExecutionEngine::make_fill_report(Order& order, Price fill_price,
                                                        Quantity fill_qty, Timestamp now) {
    order.filled_quantity += fill_qty;
    order.last_update_time = now;
    order.status = (order.remaining_quantity() == 0) ? OrderStatus::Filled
                                                       : OrderStatus::PartiallyFilled;

    ExecutionReport report;
    report.order_id = order.id;
    report.symbol = order.symbol;
    report.side = order.side;
    report.status = order.status;
    report.fill_price = fill_price;
    report.fill_quantity = fill_qty;
    report.remaining_quantity = order.remaining_quantity();
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.simulated_ack_latency;

    ++total_filled_;
    if (order.status == OrderStatus::PartiallyFilled) ++total_partial_fills_;
    return report;
}

std::vector<ExecutionReport> PaperExecutionEngine::on_market_event(const MarketEvent& ev) {
    std::vector<ExecutionReport> reports;
    if (ev.type != EventType::Trade) return reports;

    std::vector<OrderId> to_remove;
    for (auto& [id, order] : open_orders_) {
        if (order.symbol.view() != ev.symbol.view()) continue;
        if (order.type != OrderType::Limit) continue;

        bool crosses = (order.side == Side::Buy) ? (ev.price <= order.price)
                                                   : (ev.price >= order.price);
        if (!crosses) continue;

        std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
        bool partial = prob_dist(rng_) < assumptions_.partial_fill_probability;

        Quantity remaining = order.remaining_quantity();
        Quantity fill_qty = remaining;
        if (partial && remaining > 1) {
            std::uniform_real_distribution<double> ratio_dist(
                assumptions_.min_partial_fill_ratio, assumptions_.max_partial_fill_ratio);
            fill_qty = std::max<Quantity>(
                1, static_cast<Quantity>(static_cast<double>(remaining) * ratio_dist(rng_)));
        }
        fill_qty = std::min(fill_qty, ev.quantity);
        fill_qty = std::min(fill_qty, remaining);
        if (fill_qty <= 0) continue;

        reports.push_back(make_fill_report(order, order.price, fill_qty, ev.timestamp));
        if (order.remaining_quantity() == 0) {
            to_remove.push_back(id);
        }
    }
    for (auto id : to_remove) open_orders_.erase(id);
    return reports;
}

std::optional<Order> PaperExecutionEngine::get_order(OrderId id) const {
    auto it = open_orders_.find(id);
    if (it == open_orders_.end()) return std::nullopt;
    return it->second;
}

std::vector<Order> PaperExecutionEngine::open_orders() const {
    std::vector<Order> result;
    result.reserve(open_orders_.size());
    for (auto& [id, order] : open_orders_) result.push_back(order);
    return result;
}

}  // namespace lob
