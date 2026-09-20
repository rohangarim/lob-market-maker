// Benchmarks the flat-vector OrderBook (src/orderbook/order_book.hpp)
// against a std::map-based reference implementation of the same event
// semantics, across the operation mix this project actually performs:
// price-level upserts/deletes (frequent), best bid/ask reads (very
// frequent), and top-N depth reads (frequent). This is the measurement
// that justified choosing the flat-vector representation as the
// production implementation -- see the README benchmark section for the
// numbers actually produced by running this binary.

#include <benchmark/benchmark.h>

#include <map>
#include <random>

#include "orderbook/order_book.hpp"

using namespace lob;

namespace {

// Minimal std::map-based order book implementing the same event semantics
// as lob::OrderBook, kept local to this benchmark for comparison only.
class MapOrderBook {
public:
    void apply(const MarketEvent& ev) {
        switch (ev.type) {
            case EventType::Quote: {
                auto& side_map = (ev.side == Side::Buy) ? bids_ : asks_;
                if (ev.quantity == 0) {
                    side_map.erase(ev.price.ticks());
                } else {
                    side_map[ev.price.ticks()] = ev.quantity;
                }
                break;
            }
            case EventType::BookDelete: {
                auto& side_map = (ev.side == Side::Buy) ? bids_ : asks_;
                side_map.erase(ev.price.ticks());
                break;
            }
            default:
                break;
        }
    }

    [[nodiscard]] std::optional<int64_t> best_bid() const {
        if (bids_.empty()) return std::nullopt;
        return bids_.rbegin()->first;  // highest key = best bid
    }
    [[nodiscard]] std::optional<int64_t> best_ask() const {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;  // lowest key = best ask
    }

    [[nodiscard]] std::vector<std::pair<int64_t, Quantity>> bid_depth(size_t levels) const {
        std::vector<std::pair<int64_t, Quantity>> out;
        out.reserve(levels);
        for (auto it = bids_.rbegin(); it != bids_.rend() && out.size() < levels; ++it) {
            out.emplace_back(it->first, it->second);
        }
        return out;
    }

private:
    std::map<int64_t, Quantity> bids_;
    std::map<int64_t, Quantity> asks_;
};

// Generates a stream where the reference price sits on a fixed tick grid
// and moves by at most one tick on a small fraction of events (as a real
// mid-price does), rather than drifting continuously by a sub-tick amount
// on every event. This matters for this benchmark specifically: with a
// continuously-drifting reference, almost every event lands on a price
// never seen before, so the book never reaches a steady-state working set
// and the comparison would not reflect how either representation behaves
// against a real, bounded L2 book (a small number of price levels updated
// repeatedly in place).
std::vector<MarketEvent> make_realistic_update_stream(size_t n, int levels_per_side = 20) {
    std::mt19937_64 rng(4242);
    std::uniform_real_distribution<double> unit(0.0, 1.0);
    std::uniform_int_distribution<Quantity> qty_dist(1, 500);
    constexpr int64_t kTickTicks = 100;  // $0.01 in Price internal-tick units
    int64_t mid_ticks = 100 * Price::kScale;  // reference: $100.00
    std::vector<MarketEvent> events;
    events.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        if (unit(rng) < 0.05) {
            mid_ticks += (unit(rng) < 0.5 ? -1 : 1) * kTickTicks;
        }
        Side side = unit(rng) < 0.5 ? Side::Buy : Side::Sell;
        int level = static_cast<int>(std::pow(unit(rng), 2.0) * levels_per_side);
        int64_t px_ticks = side == Side::Buy ? mid_ticks - kTickTicks * (level + 1)
                                              : mid_ticks + kTickTicks * (level + 1);
        events.push_back(MarketEvent::make_quote(Timestamp(static_cast<int64_t>(i)),
                                                   static_cast<SequenceNumber>(i + 1), Symbol("AAPL"),
                                                   side, Price::from_ticks(px_ticks), qty_dist(rng)));
    }
    return events;
}

}  // namespace

static void BM_FlatOrderBook_ApplyUpdates(benchmark::State& state) {
    auto events = make_realistic_update_stream(10'000);
    for (auto _ : state) {
        OrderBook book(Symbol("AAPL"));
        for (const auto& ev : events) book.apply(ev);
        benchmark::DoNotOptimize(book.best_bid());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * events.size()));
}
BENCHMARK(BM_FlatOrderBook_ApplyUpdates);

static void BM_MapOrderBook_ApplyUpdates(benchmark::State& state) {
    auto events = make_realistic_update_stream(10'000);
    for (auto _ : state) {
        MapOrderBook book;
        for (const auto& ev : events) book.apply(ev);
        benchmark::DoNotOptimize(book.best_bid());
    }
    state.SetItemsProcessed(static_cast<int64_t>(state.iterations() * events.size()));
}
BENCHMARK(BM_MapOrderBook_ApplyUpdates);

static void BM_FlatOrderBook_BestBidAskReads(benchmark::State& state) {
    auto events = make_realistic_update_stream(2'000);
    OrderBook book(Symbol("AAPL"));
    for (const auto& ev : events) book.apply(ev);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best_bid());
        benchmark::DoNotOptimize(book.best_ask());
    }
}
BENCHMARK(BM_FlatOrderBook_BestBidAskReads);

static void BM_MapOrderBook_BestBidAskReads(benchmark::State& state) {
    auto events = make_realistic_update_stream(2'000);
    MapOrderBook book;
    for (const auto& ev : events) book.apply(ev);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.best_bid());
        benchmark::DoNotOptimize(book.best_ask());
    }
}
BENCHMARK(BM_MapOrderBook_BestBidAskReads);

static void BM_FlatOrderBook_Depth10Reads(benchmark::State& state) {
    auto events = make_realistic_update_stream(2'000);
    OrderBook book(Symbol("AAPL"));
    for (const auto& ev : events) book.apply(ev);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.bid_depth(10));
    }
}
BENCHMARK(BM_FlatOrderBook_Depth10Reads);

static void BM_MapOrderBook_Depth10Reads(benchmark::State& state) {
    auto events = make_realistic_update_stream(2'000);
    MapOrderBook book;
    for (const auto& ev : events) book.apply(ev);
    for (auto _ : state) {
        benchmark::DoNotOptimize(book.bid_depth(10));
    }
}
BENCHMARK(BM_MapOrderBook_Depth10Reads);

BENCHMARK_MAIN();
