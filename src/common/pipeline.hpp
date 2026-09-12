#pragma once

#include <optional>

#include "execution/paper_execution_engine.hpp"
#include "metrics/latency_stats.hpp"
#include "metrics/microstructure.hpp"
#include "orderbook/order_book.hpp"
#include "portfolio/portfolio.hpp"
#include "risk/risk_engine.hpp"
#include "strategy/market_maker.hpp"

namespace lob {

struct PipelineConfig {
    RiskLimits risk_limits{};
    StrategyConfig strategy{};
    FillAssumptions fill_assumptions{};
};

struct PipelineStats {
    size_t events_processed = 0;
    size_t sequence_gaps = 0;
    size_t quotes_submitted = 0;
    size_t quotes_canceled = 0;
    size_t fills = 0;
    size_t partial_fills = 0;
    size_t risk_rejects = 0;
    LatencyStats decision_latency;  // signal calc -> strategy -> risk check -> order creation
};

// ---------------------------------------------------------------------------
// The shared processing pipeline: order book -> microstructure -> strategy
// -> risk -> paper execution -> portfolio. Both the live-mode consumer and
// the replay engine drive the exact same Pipeline::process() for every
// MarketEvent, which is what makes replay a faithful reproduction of what
// live mode would have done given the same event stream (see docs on
// determinism in the replay engine).
//
// Single-owner: process() must be called from one thread only (the
// order-book/strategy thread in the multithreading model described in
// docs/architecture.md).
// ---------------------------------------------------------------------------
class Pipeline {
public:
    Pipeline(Symbol symbol, PipelineConfig cfg)
        : book_(symbol),
          risk_(cfg.risk_limits),
          strategy_(cfg.strategy),
          exec_(cfg.fill_assumptions),
          stale_threshold_sec_(cfg.strategy.max_staleness_sec) {}

    void process(const MarketEvent& ev) {
        ++stats_.events_processed;

        if (last_sequence_ && ev.sequence != *last_sequence_ + 1) {
            ++stats_.sequence_gaps;
        }
        last_sequence_ = ev.sequence;

        book_.apply(ev);
        micro_.on_book_update(book_, ev.timestamp);
        if (ev.type == EventType::Trade) {
            micro_.on_trade(ev.quantity, ev.timestamp);
        }

        for (const auto& report : exec_.on_market_event(ev)) {
            apply_report(report);
        }

        if (auto mid = book_.mid_price()) {
            portfolio_.mark_to_market(*mid);
        }

        bool book_changed = (ev.type == EventType::Quote || ev.type == EventType::BookDelete ||
                              ev.type == EventType::Snapshot);
        if (book_changed) {
            requote(ev.timestamp);
        }
    }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const Portfolio& portfolio() const noexcept { return portfolio_; }
    [[nodiscard]] const PipelineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PaperExecutionEngine& execution() const noexcept { return exec_; }
    [[nodiscard]] const MicrostructureCalculator& microstructure() const noexcept { return micro_; }

private:
    struct RestingQuote {
        OrderId id;
        Price price;
        Quantity size;
    };

    void apply_report(const ExecutionReport& report) {
        if (report.fill_quantity > 0) {
            portfolio_.on_fill(report.side, report.fill_price, report.fill_quantity);
            ++stats_.fills;
            if (report.status == OrderStatus::PartiallyFilled) ++stats_.partial_fills;
        }
        if (report.status == OrderStatus::Filled) {
            if (resting_bid_ && resting_bid_->id == report.order_id) resting_bid_.reset();
            if (resting_ask_ && resting_ask_->id == report.order_id) resting_ask_.reset();
        }
    }

    void requote(Timestamp now) {
        auto t0 = std::chrono::steady_clock::now();

        double staleness_sec =
            static_cast<double>((now - book_.last_update_time()).count()) / 1e9;
        bool stale = staleness_sec > stale_threshold_sec_;

        QuoteDecision qd = strategy_.decide(book_, micro_, portfolio_.state().position, now);

        if (!qd.quote_bid && resting_bid_) {
            exec_.cancel(resting_bid_->id, now);
            resting_bid_.reset();
            ++stats_.quotes_canceled;
        }
        if (!qd.quote_ask && resting_ask_) {
            exec_.cancel(resting_ask_->id, now);
            resting_ask_.reset();
            ++stats_.quotes_canceled;
        }

        auto mid = book_.mid_price();
        double reference_price = mid.value_or(0.0);

        if (qd.quote_bid && (!resting_bid_ || resting_bid_->price != qd.bid_price ||
                              resting_bid_->size != qd.bid_size)) {
            if (resting_bid_) {
                exec_.cancel(resting_bid_->id, now);
                resting_bid_.reset();
                ++stats_.quotes_canceled;
            }
            RiskCheckInput in{qd.bid_size, Side::Buy, reference_price,
                               portfolio_.state().position, portfolio_.state().total_pnl,
                               exec_.open_order_count(), stale};
            if (risk_.check(in) == RiskDecision::Approved) {
                OrderId id = next_order_id_++;
                exec_.submit_limit(id, book_.symbol(), Side::Buy, qd.bid_price, qd.bid_size, now);
                resting_bid_ = RestingQuote{id, qd.bid_price, qd.bid_size};
                ++stats_.quotes_submitted;
            } else {
                ++stats_.risk_rejects;
            }
        }

        if (qd.quote_ask && (!resting_ask_ || resting_ask_->price != qd.ask_price ||
                              resting_ask_->size != qd.ask_size)) {
            if (resting_ask_) {
                exec_.cancel(resting_ask_->id, now);
                resting_ask_.reset();
                ++stats_.quotes_canceled;
            }
            RiskCheckInput in{qd.ask_size, Side::Sell, reference_price,
                               portfolio_.state().position, portfolio_.state().total_pnl,
                               exec_.open_order_count(), stale};
            if (risk_.check(in) == RiskDecision::Approved) {
                OrderId id = next_order_id_++;
                exec_.submit_limit(id, book_.symbol(), Side::Sell, qd.ask_price, qd.ask_size, now);
                resting_ask_ = RestingQuote{id, qd.ask_price, qd.ask_size};
                ++stats_.quotes_submitted;
            } else {
                ++stats_.risk_rejects;
            }
        }

        auto t1 = std::chrono::steady_clock::now();
        stats_.decision_latency.add_ns(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    OrderBook book_;
    MicrostructureCalculator micro_;
    RiskEngine risk_;
    MarketMakerStrategy strategy_;
    PaperExecutionEngine exec_;
    Portfolio portfolio_;

    double stale_threshold_sec_;
    OrderId next_order_id_ = 1;
    std::optional<SequenceNumber> last_sequence_;
    std::optional<RestingQuote> resting_bid_;
    std::optional<RestingQuote> resting_ask_;
    PipelineStats stats_;
};

}  // namespace lob
