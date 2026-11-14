#pragma once

#include <algorithm>
#include <chrono>
#include <functional>
#include <optional>
#include <vector>

#include "execution/paper_execution_engine.hpp"
#include "metrics/latency_stats.hpp"
#include "metrics/markout.hpp"
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
    // Opt-in: time the book-update and execution stages of every event. Costs
    // a few extra clock reads per event, so it is off by default.
    bool profile_stages = false;
    // Decision latency is recorded for every Nth requote (1 = every one). Reading the
    // clock twice per requote was measured to cost about as much as the work being timed,
    // so the default samples; the percentiles are an unbiased sample of the same
    // distribution because the sampling is a fixed stride, not data-dependent.
    uint32_t latency_sample_interval = 16;
    // Event-time horizons at which fills are marked to the mid (adverse selection).
    std::vector<Timestamp> markout_horizons = {
        std::chrono::milliseconds(1), std::chrono::milliseconds(10),
        std::chrono::milliseconds(100), std::chrono::seconds(1)};
};

struct PipelineStats {
    size_t events_processed = 0;       // every event handed to process(), including dropped ones
    size_t out_of_order_dropped = 0;   // sequence <= last seen (duplicates and reordering); not applied
    size_t sequence_gaps = 0;          // sequence jumped forward; triggers a book resync
    size_t stale_resets = 0;           // event-time gap above the staleness threshold; triggers a resync
    size_t clock_stale_events = 0;     // on_clock() found the feed silent beyond the threshold
    size_t crossed_book_events = 0;    // events after which best bid >= best ask; quoting suppressed
    size_t quotes_submitted = 0;
    size_t quotes_canceled = 0;
    size_t fills = 0;
    size_t partial_fills = 0;
    size_t risk_rejects = 0;
    LatencyStats decision_latency;     // strategy -> risk -> order creation (always measured)
    LatencyStats book_update_latency;  // only populated when profile_stages is set
    LatencyStats execution_latency;    // only populated when profile_stages is set
};

// ---------------------------------------------------------------------------
// The shared processing pipeline: order book -> microstructure -> strategy ->
// risk -> paper execution -> portfolio. Live mode and replay mode both call
// Pipeline::process() for every MarketEvent.
//
// Feed-integrity policy (all of it deterministic and driven only by event
// content/timestamps, so replay stays reproducible):
//   - sequence <= last seen:  the event is dropped and counted; it is never
//                             applied to the book.
//   - sequence jumps forward: updates were missed, so the book can no longer
//                             be trusted. Resting quotes are canceled and the
//                             book is cleared; it rebuilds from later updates.
//   - event-time gap above max_staleness_sec since the previous event: same
//                             resync (the old levels are too old to trust).
//   - book crossed after an event: resting quotes are canceled and no new
//                             quotes are generated until it is uncrossed.
//   - on_clock(now): lets a live loop pass wall-clock time so a feed that has
//                             gone silent (no events at all) also cancels
//                             quotes. Replay never calls it.
//
// Single-owner: call process()/on_clock() from one thread only.
// ---------------------------------------------------------------------------
class Pipeline {
public:
    Pipeline(Symbol symbol, PipelineConfig cfg)
        : book_(symbol),
          risk_(cfg.risk_limits),
          strategy_(cfg.strategy),
          exec_(cfg.fill_assumptions),
          stale_threshold_(std::chrono::duration_cast<Timestamp>(
              std::chrono::duration<double>(cfg.strategy.max_staleness_sec))),
          requote_threshold_bps_(cfg.strategy.requote_threshold_bps),
          sample_interval_(std::max<uint32_t>(1, cfg.latency_sample_interval)),
          profile_stages_(cfg.profile_stages),
          markout_(cfg.markout_horizons) {}

    void process(const MarketEvent& ev) {
        ++stats_.events_processed;

        if (last_sequence_ && ev.sequence <= *last_sequence_) {
            ++stats_.out_of_order_dropped;
            return;
        }
        bool gap = last_sequence_ && ev.sequence != *last_sequence_ + 1;
        bool stale = last_event_ts_ && (ev.timestamp - *last_event_ts_) > stale_threshold_;
        last_sequence_ = ev.sequence;
        last_event_ts_ = ev.timestamp;
        market_data_stale_ = false;

        if (gap) ++stats_.sequence_gaps;
        if (stale) ++stats_.stale_resets;
        if (gap || stale) resync(ev.timestamp);

        if (last_mid_) markout_.on_time(ev.timestamp, *last_mid_);

        auto t0 = profile_stages_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        book_.apply(ev);
        micro_.on_book_update(book_, ev.timestamp);
        if (ev.type == EventType::Trade) micro_.on_trade(ev.quantity, ev.timestamp);
        auto t1 = profile_stages_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

        last_mid_ = book_.mid_price();
        for (const auto& report : exec_.on_market_event(ev)) apply_report(report);
        if (last_mid_) portfolio_.mark_to_market(*last_mid_);
        analytics_.sample(portfolio_.state().position, portfolio_.state().total_pnl);
        if (profile_stages_) {
            auto t2 = std::chrono::steady_clock::now();
            stats_.book_update_latency.add_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            stats_.execution_latency.add_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count());
        }

        bool crossed = book_.crossed();
        if (crossed) {
            ++stats_.crossed_book_events;
            cancel_resting_quotes(ev.timestamp);
            return;
        }

        bool book_changed = (ev.type == EventType::Quote || ev.type == EventType::BookDelete ||
                              ev.type == EventType::Snapshot);
        if (book_changed) requote(ev.timestamp);
    }

    // Live loops call this periodically with wall-clock time. If the feed has
    // produced nothing for longer than the staleness threshold, quotes are
    // canceled and new ones are rejected by the risk engine until the next
    // event arrives. Event timestamps and `now` must share an epoch.
    void on_clock(Timestamp now) {
        if (!last_event_ts_ || market_data_stale_) return;
        if (now - *last_event_ts_ > stale_threshold_) {
            market_data_stale_ = true;
            ++stats_.clock_stale_events;
            cancel_resting_quotes(now);
        }
    }

    // Optional instrumentation: called for every fill with the mid that was in force
    // when it happened. Unset by default (no cost on the hot path beyond a null check).
    using FillObserver = std::function<void(const ExecutionReport&, std::optional<double> mid)>;
    void set_fill_observer(FillObserver obs) { fill_observer_ = std::move(obs); }

    [[nodiscard]] const OrderBook& book() const noexcept { return book_; }
    [[nodiscard]] const Portfolio& portfolio() const noexcept { return portfolio_; }
    [[nodiscard]] const PipelineStats& stats() const noexcept { return stats_; }
    [[nodiscard]] const PaperExecutionEngine& execution() const noexcept { return exec_; }
    [[nodiscard]] const MicrostructureCalculator& microstructure() const noexcept { return micro_; }
    [[nodiscard]] bool market_data_stale() const noexcept { return market_data_stale_; }
    [[nodiscard]] uint32_t latency_sample_interval() const noexcept { return sample_interval_; }
    [[nodiscard]] const MarkoutTracker& markout() const noexcept { return markout_; }
    [[nodiscard]] const RunAnalytics& analytics() const noexcept { return analytics_; }

private:
    struct RestingQuote {
        OrderId id;
        Price price;
        Quantity size;
    };

    void apply_report(const ExecutionReport& report) {
        if (report.fill_quantity > 0) {
            portfolio_.on_fill(report.side, report.fill_price, report.fill_quantity, report.fee);
            if (fill_observer_) fill_observer_(report, last_mid_);
            if (last_mid_) {
                markout_.on_fill(report.side, report.fill_price.to_double(), report.fill_quantity,
                                 *last_mid_, report.timestamp);
            }
            ++stats_.fills;
            if (report.status == OrderStatus::PartiallyFilled) ++stats_.partial_fills;
        }
        if (report.status == OrderStatus::Filled) {
            if (resting_bid_ && resting_bid_->id == report.order_id) resting_bid_.reset();
            if (resting_ask_ && resting_ask_->id == report.order_id) resting_ask_.reset();
        }
    }

    void cancel_side(std::optional<RestingQuote>& quote, Timestamp now) {
        if (!quote) return;
        exec_.cancel(quote->id, now);
        quote.reset();
        ++stats_.quotes_canceled;
    }

    void cancel_resting_quotes(Timestamp now) {
        cancel_side(resting_bid_, now);
        cancel_side(resting_ask_, now);
    }

    void resync(Timestamp now) {
        cancel_resting_quotes(now);
        book_.clear();
        micro_.reset_price_history();
    }

    void requote(Timestamp now) {
        const bool timed = (++requotes_ % sample_interval_) == 0;
        std::chrono::steady_clock::time_point t0;
        if (timed) t0 = std::chrono::steady_clock::now();

        QuoteDecision qd = strategy_.decide(book_, micro_, portfolio_.state().position, now);
        double reference_price = book_.mid_price().value_or(0.0);
        const int64_t threshold_ticks = static_cast<int64_t>(
            reference_price * requote_threshold_bps_ / 10'000.0 * static_cast<double>(Price::kScale));

        if (!qd.quote_bid) cancel_side(resting_bid_, now);
        if (!qd.quote_ask) cancel_side(resting_ask_, now);

        if (qd.quote_bid && needs_replace(resting_bid_, qd.bid_price, qd.bid_size, threshold_ticks)) {
            cancel_side(resting_bid_, now);
            submit(Side::Buy, qd.bid_price, qd.bid_size, reference_price, now, resting_bid_);
        }
        if (qd.quote_ask && needs_replace(resting_ask_, qd.ask_price, qd.ask_size, threshold_ticks)) {
            cancel_side(resting_ask_, now);
            submit(Side::Sell, qd.ask_price, qd.ask_size, reference_price, now, resting_ask_);
        }

        if (timed) {
            auto t1 = std::chrono::steady_clock::now();
            stats_.decision_latency.add_ns(
                std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
        }
    }

    static bool needs_replace(const std::optional<RestingQuote>& resting, Price target, Quantity size,
                              int64_t threshold_ticks) {
        if (!resting || resting->size != size) return true;
        int64_t move = resting->price.ticks() - target.ticks();
        return (move < 0 ? -move : move) > threshold_ticks;
    }

    void submit(Side side, Price price, Quantity size, double reference_price, Timestamp now,
                std::optional<RestingQuote>& slot) {
        RiskCheckInput in{size, side, reference_price, portfolio_.state().position,
                           portfolio_.state().total_pnl, exec_.open_order_count(),
                           market_data_stale_};
        if (risk_.check(in) != RiskDecision::Approved) {
            ++stats_.risk_rejects;
            return;
        }
        OrderId id = next_order_id_++;
        SubmitResult res = exec_.submit_limit(id, book_.symbol(), side, price, size, now, book_);
        ++stats_.quotes_submitted;
        if (res.taker_fill) apply_report(*res.taker_fill);
        if (exec_.get_order(id)) slot = RestingQuote{id, price, size};
    }

    OrderBook book_;
    MicrostructureCalculator micro_;
    RiskEngine risk_;
    MarketMakerStrategy strategy_;
    PaperExecutionEngine exec_;
    Portfolio portfolio_;

    Timestamp stale_threshold_;
    double requote_threshold_bps_;
    uint32_t sample_interval_;
    uint32_t requotes_ = 0;
    bool profile_stages_;
    bool market_data_stale_ = false;
    OrderId next_order_id_ = 1;
    std::optional<SequenceNumber> last_sequence_;
    std::optional<Timestamp> last_event_ts_;
    std::optional<RestingQuote> resting_bid_;
    std::optional<RestingQuote> resting_ask_;
    PipelineStats stats_;
    MarkoutTracker markout_;
    RunAnalytics analytics_;
    std::optional<double> last_mid_;
    FillObserver fill_observer_;
};

}  // namespace lob
