#include "execution/paper_execution_engine.hpp"

#include <algorithm>
#include <cmath>

namespace lob {

Order* PaperExecutionEngine::find(OrderId id) {
    for (auto& o : open_orders_) {
        if (o.id == id) return &o;
    }
    return nullptr;
}

ExecutionReport PaperExecutionEngine::submit_limit(OrderId id, Symbol symbol, Side side, Price price,
                                                    Quantity quantity, Timestamp now) {
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
    order.active_time = now + assumptions_.order_latency;
    open_orders_.push_back(order);

    ExecutionReport report;
    report.order_id = id;
    report.symbol = symbol;
    report.side = side;
    report.status = OrderStatus::New;
    report.fill_price = price;
    report.remaining_quantity = quantity;
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.order_latency;
    return report;
}

SubmitResult PaperExecutionEngine::submit_limit(OrderId id, Symbol symbol, Side side, Price price,
                                                 Quantity quantity, Timestamp now,
                                                 const OrderBook& book) {
    SubmitResult result;

    // 1. Take visible liquidity if the order is marketable on arrival.
    Quantity remaining = quantity;
    Quantity taken = 0;
    double notional = 0.0;
    auto opposite = (side == Side::Buy) ? book.asks() : book.bids();
    for (const auto& level : opposite) {
        bool crosses = (side == Side::Buy) ? level.price <= price : level.price >= price;
        if (!crosses || remaining <= 0) break;
        Quantity take = std::min(remaining, level.quantity);
        taken += take;
        notional += static_cast<double>(take) * level.price.to_double();
        remaining -= take;
    }

    if (taken == 0) {
        result.ack = submit_limit(id, symbol, side, price, quantity, now);
        if (Order* o = find(id)) {
            Quantity ahead = book.quantity_at(side, price);
            o->queue_ahead = static_cast<double>(ahead);
            o->level_qty_seen = ahead;
        }
        return result;
    }

    ++total_submitted_;
    Order order;
    order.id = id;
    order.symbol = symbol;
    order.side = side;
    order.type = OrderType::Limit;
    order.price = price;
    order.quantity = quantity;
    order.created_time = now;
    order.active_time = now + assumptions_.order_latency;
    ExecutionReport fill = make_fill_report(order, Price::from_double(notional / static_cast<double>(taken)),
                                             taken, now, /*taker=*/true);
    ++total_taker_fills_;
    result.taker_fill = fill;

    result.ack = fill;
    result.ack.status = OrderStatus::New;
    result.ack.fill_quantity = 0;
    result.ack.fee = 0.0;
    result.ack.taker = false;
    result.ack.remaining_quantity = quantity;

    if (order.remaining_quantity() > 0) {
        Quantity ahead = book.quantity_at(side, price);
        order.queue_ahead = static_cast<double>(ahead);
        order.level_qty_seen = ahead;
        open_orders_.push_back(order);
    }
    return result;
}

ExecutionReport PaperExecutionEngine::submit_market(OrderId id, Symbol symbol, Side side,
                                                     Quantity quantity, const OrderBook& book,
                                                     Timestamp now) {
    ++total_submitted_;
    Quantity remaining = quantity;
    Quantity filled = 0;
    double notional = 0.0;
    auto opposite = (side == Side::Buy) ? book.asks() : book.bids();
    for (const auto& level : opposite) {
        if (remaining <= 0) break;
        Quantity take = std::min(remaining, level.quantity);
        filled += take;
        notional += static_cast<double>(take) * level.price.to_double();
        remaining -= take;
    }

    ExecutionReport report;
    report.order_id = id;
    report.symbol = symbol;
    report.side = side;
    report.status = (filled == quantity) ? OrderStatus::Filled
                    : (filled > 0)       ? OrderStatus::PartiallyFilled
                                         : OrderStatus::Rejected;
    report.fill_price = (filled > 0) ? Price::from_double(notional / static_cast<double>(filled)) : Price{};
    report.fill_quantity = filled;
    report.remaining_quantity = 0;  // market orders never rest
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.order_latency;
    report.taker = true;
    report.fee = assumptions_.taker_fee_per_share * static_cast<double>(filled);
    if (filled == 0) report.reason = "no opposite-side liquidity available";
    if (filled > 0) {
        ++total_filled_;
        ++total_taker_fills_;
        if (filled < quantity) ++total_partial_fills_;
    }
    return report;
}

ExecutionReport PaperExecutionEngine::make_cancel_report(const Order& order, Timestamp now) const {
    ExecutionReport report;
    report.order_id = order.id;
    report.symbol = order.symbol;
    report.side = order.side;
    report.status = OrderStatus::Canceled;
    report.remaining_quantity = order.remaining_quantity();
    report.timestamp = now;
    report.submit_to_ack_latency = assumptions_.cancel_latency;
    return report;
}

std::optional<ExecutionReport> PaperExecutionEngine::cancel(OrderId id, Timestamp now) {
    auto it = std::find_if(open_orders_.begin(), open_orders_.end(),
                           [id](const Order& o) { return o.id == id; });
    if (it == open_orders_.end() || it->cancel_pending) return std::nullopt;

    if (assumptions_.cancel_latency.count() > 0) {
        it->cancel_pending = true;
        it->cancel_effective = now + assumptions_.cancel_latency;
        return std::nullopt;
    }
    ExecutionReport report = make_cancel_report(*it, now);
    open_orders_.erase(it);
    ++total_canceled_;
    return report;
}

ExecutionReport PaperExecutionEngine::make_fill_report(Order& order, Price fill_price,
                                                        Quantity fill_qty, Timestamp now, bool taker) {
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
    report.submit_to_ack_latency = assumptions_.order_latency;
    report.taker = taker;
    report.fee = (taker ? assumptions_.taker_fee_per_share : assumptions_.maker_fee_per_share) *
                 static_cast<double>(fill_qty);

    ++total_filled_;
    if (order.status == OrderStatus::PartiallyFilled) ++total_partial_fills_;
    return report;
}

void PaperExecutionEngine::update_level(Order& order, Quantity new_level_qty) {
    Quantity expected = std::max<Quantity>(0, order.level_qty_seen - order.trades_since_update);
    order.trades_since_update = 0;
    if (new_level_qty < expected && expected > 0) {
        order.queue_ahead *= static_cast<double>(new_level_qty) / static_cast<double>(expected);
    }
    order.level_qty_seen = new_level_qty;
}

Quantity PaperExecutionEngine::eligible_fill_queue_model(Order& order, const MarketEvent& ev) {
    const bool through = (order.side == Side::Buy) ? ev.price < order.price : ev.price > order.price;
    const bool active = ev.timestamp >= order.active_time;

    if (through) {
        if (!active) return 0;
        order.queue_ahead = 0.0;
        return order.remaining_quantity();
    }
    if (ev.price != order.price) return 0;

    // The queue ahead is consumed whether or not we were live yet.
    order.trades_since_update += ev.quantity;
    double q = static_cast<double>(ev.quantity);
    if (order.queue_ahead >= q) {
        order.queue_ahead -= q;
        return 0;
    }
    double excess = q - order.queue_ahead;
    order.queue_ahead = 0.0;
    return active ? static_cast<Quantity>(std::floor(excess)) : 0;
}

Quantity PaperExecutionEngine::eligible_fill_trade_through(const Order& order, const MarketEvent& ev) {
    if (ev.timestamp < order.active_time) return 0;
    bool crosses = (order.side == Side::Buy) ? (ev.price <= order.price) : (ev.price >= order.price);
    if (!crosses) return 0;

    std::uniform_real_distribution<double> prob_dist(0.0, 1.0);
    bool partial = prob_dist(rng_) < assumptions_.partial_fill_probability;
    Quantity remaining = order.remaining_quantity();
    Quantity fill_qty = remaining;
    if (partial && remaining > 1) {
        std::uniform_real_distribution<double> ratio_dist(assumptions_.min_partial_fill_ratio,
                                                            assumptions_.max_partial_fill_ratio);
        fill_qty = std::max<Quantity>(
            1, static_cast<Quantity>(static_cast<double>(remaining) * ratio_dist(rng_)));
    }
    return std::min(fill_qty, ev.quantity);
}

std::vector<ExecutionReport> PaperExecutionEngine::on_market_event(const MarketEvent& ev) {
    std::vector<ExecutionReport> reports;
    if (open_orders_.empty()) return reports;

    // Cancels whose effective time has arrived take effect before this event.
    for (auto it = open_orders_.begin(); it != open_orders_.end();) {
        if (it->cancel_pending && ev.timestamp >= it->cancel_effective) {
            reports.push_back(make_cancel_report(*it, ev.timestamp));
            ++total_canceled_;
            it = open_orders_.erase(it);
        } else {
            ++it;
        }
    }

    const bool queue_model = assumptions_.fill_model == FillModel::QueuePosition;

    if (ev.type == EventType::Trade) {
        for (auto& order : open_orders_) {
            if (order.symbol.view() != ev.symbol.view() || order.type != OrderType::Limit) continue;
            Quantity eligible = queue_model ? eligible_fill_queue_model(order, ev)
                                            : eligible_fill_trade_through(order, ev);
            Quantity fill_qty = std::min(eligible, order.remaining_quantity());
            if (fill_qty <= 0) continue;
            reports.push_back(make_fill_report(order, order.price, fill_qty, ev.timestamp, false));
        }
        std::erase_if(open_orders_, [](const Order& o) { return o.remaining_quantity() == 0; });
    } else if (queue_model) {
        for (auto& order : open_orders_) {
            if (order.symbol.view() != ev.symbol.view()) continue;
            if (ev.type == EventType::Snapshot) {
                order.queue_ahead = 0.0;
                order.level_qty_seen = 0;
                order.trades_since_update = 0;
            } else if ((ev.type == EventType::Quote || ev.type == EventType::BookDelete) &&
                       ev.side == order.side && ev.price == order.price) {
                update_level(order, ev.type == EventType::BookDelete ? 0 : ev.quantity);
                if (ev.type == EventType::BookDelete) order.queue_ahead = 0.0;
            }
        }
    }
    return reports;
}

std::optional<Order> PaperExecutionEngine::get_order(OrderId id) const {
    for (const auto& o : open_orders_) {
        if (o.id == id) return o;
    }
    return std::nullopt;
}

}  // namespace lob
