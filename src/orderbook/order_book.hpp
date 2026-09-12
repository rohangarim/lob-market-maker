#pragma once

#include <algorithm>
#include <optional>
#include <vector>

#include "common/types.hpp"
#include "marketdata/events.hpp"

namespace lob {

struct BookLevel {
    Price price{};
    Quantity quantity = 0;
};

// ---------------------------------------------------------------------------
// In-memory limit order book, one instance per symbol.
//
// Representation choice: each side is a flat, contiguous std::vector<BookLevel>
// kept sorted by price (bids descending, asks ascending), maintained with
// std::lower_bound + insert/erase rather than a node-based std::map.
//
// This was a deliberate choice made after benchmarking both representations
// (see benchmarks/bench_order_book.cpp and the README benchmark section):
// real order books rarely carry more than a few dozen resting price levels
// per side, and the operations this project performs most often -- read
// best bid/ask, read top-N depth, iterate the whole side for imbalance --
// are exactly the operations a flat sorted array is best at: they are
// sequential scans over contiguous memory with no pointer chasing, so they
// stay in L1 and vectorize well. std::map's O(log n) lookup only wins when
// n is large and lookups dominate over full scans/iteration, which is not
// this workload. The O(n) insert/erase cost of shifting a vector is paid
// only on the (relatively rarer) new-price-level path, and n is small
// enough that a memmove of a few dozen 16-byte elements is still faster in
// practice than a heap allocation + red-black tree rebalance.
//
// Threading: single-writer. Exactly one thread (the order-book/strategy
// thread) may call apply(); readers (best_bid/best_ask/depth/snapshot) are
// expected to be called from that same thread. No internal locking is
// performed -- see docs/architecture.md for the thread ownership model.
//
// Event semantics:
//   Quote      -- upsert absolute resting quantity at (side, price). A
//                 quantity of 0 removes the level. This matches the
//                 common normalized L2 "add/update/delete by size" model
//                 used by most consolidated feeds.
//   BookDelete -- explicit removal of a level, used when a feed reports
//                 deletions distinctly from zero-size updates.
//   Trade      -- does NOT mutate price levels directly (trade prints and
//                 quote/depth updates are independent normalized streams
//                 in most real feeds, including Finnhub's trade-only free
//                 tier used by this project's live mode). Trades update
//                 last-trade state consumed by the microstructure module
//                 for rolling volume / trade intensity.
//   Snapshot   -- clears both sides; the caller then replays a batch of
//                 Quote events to repopulate. Used at replay/session start.
// ---------------------------------------------------------------------------
class OrderBook {
public:
    explicit OrderBook(Symbol symbol) : symbol_(symbol) {
        bids_.reserve(64);
        asks_.reserve(64);
    }

    void apply(const MarketEvent& ev) {
        last_sequence_ = ev.sequence;
        last_update_ = ev.timestamp;
        switch (ev.type) {
            case EventType::Quote:
                upsert(ev.side, ev.price, ev.quantity);
                break;
            case EventType::BookDelete:
                erase(ev.side, ev.price);
                break;
            case EventType::Trade:
                last_trade_price_ = ev.price;
                last_trade_qty_ = ev.quantity;
                last_trade_time_ = ev.timestamp;
                break;
            case EventType::Snapshot:
                bids_.clear();
                asks_.clear();
                break;
            case EventType::Heartbeat:
                break;
        }
    }

    [[nodiscard]] std::optional<Price> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.front().price;
    }

    [[nodiscard]] std::optional<Price> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.front().price;
    }

    [[nodiscard]] std::optional<Quantity> best_bid_quantity() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.front().quantity;
    }

    [[nodiscard]] std::optional<Quantity> best_ask_quantity() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.front().quantity;
    }

    [[nodiscard]] std::optional<double> mid_price() const noexcept {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return (bb->to_double() + ba->to_double()) / 2.0;
    }

    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto bb = best_bid();
        auto ba = best_ask();
        if (!bb || !ba) return std::nullopt;
        return ba->to_double() - bb->to_double();
    }

    [[nodiscard]] bool crossed() const noexcept {
        auto bb = best_bid();
        auto ba = best_ask();
        return bb && ba && bb->ticks() >= ba->ticks();
    }

    [[nodiscard]] std::vector<BookLevel> bid_depth(size_t levels) const {
        size_t n = std::min(levels, bids_.size());
        return std::vector<BookLevel>(bids_.begin(), bids_.begin() + static_cast<long>(n));
    }

    [[nodiscard]] std::vector<BookLevel> ask_depth(size_t levels) const {
        size_t n = std::min(levels, asks_.size());
        return std::vector<BookLevel>(asks_.begin(), asks_.begin() + static_cast<long>(n));
    }

    struct Snapshot {
        Symbol symbol;
        SequenceNumber sequence;
        Timestamp timestamp;
        std::vector<BookLevel> bids;
        std::vector<BookLevel> asks;
    };

    [[nodiscard]] Snapshot snapshot() const {
        return Snapshot{symbol_, last_sequence_, last_update_, bids_, asks_};
    }

    [[nodiscard]] Symbol symbol() const noexcept { return symbol_; }
    [[nodiscard]] SequenceNumber last_sequence() const noexcept { return last_sequence_; }
    [[nodiscard]] Timestamp last_update_time() const noexcept { return last_update_; }
    [[nodiscard]] std::optional<Price> last_trade_price() const noexcept { return last_trade_price_; }
    [[nodiscard]] std::optional<Quantity> last_trade_quantity() const noexcept { return last_trade_qty_; }
    [[nodiscard]] std::optional<Timestamp> last_trade_time() const noexcept { return last_trade_time_; }

    [[nodiscard]] size_t bid_level_count() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_level_count() const noexcept { return asks_.size(); }

private:
    void upsert(Side side, Price price, Quantity qty) {
        auto& levels = (side == Side::Buy) ? bids_ : asks_;
        auto it = find_position(levels, side, price);
        if (it != levels.end() && it->price == price) {
            if (qty == 0) {
                levels.erase(it);
            } else {
                it->quantity = qty;
            }
        } else if (qty != 0) {
            levels.insert(it, BookLevel{price, qty});
        }
    }

    void erase(Side side, Price price) {
        auto& levels = (side == Side::Buy) ? bids_ : asks_;
        auto it = find_position(levels, side, price);
        if (it != levels.end() && it->price == price) {
            levels.erase(it);
        }
    }

    static std::vector<BookLevel>::iterator find_position(std::vector<BookLevel>& levels,
                                                            Side side, Price price) {
        if (side == Side::Buy) {
            // descending order: first element with price <= target
            return std::lower_bound(levels.begin(), levels.end(), price,
                                     [](const BookLevel& lvl, Price p) { return lvl.price > p; });
        }
        // ascending order: first element with price >= target
        return std::lower_bound(levels.begin(), levels.end(), price,
                                 [](const BookLevel& lvl, Price p) { return lvl.price < p; });
    }

    Symbol symbol_;
    std::vector<BookLevel> bids_;  // descending by price
    std::vector<BookLevel> asks_;  // ascending by price
    SequenceNumber last_sequence_ = 0;
    Timestamp last_update_{};
    std::optional<Price> last_trade_price_;
    std::optional<Quantity> last_trade_qty_;
    std::optional<Timestamp> last_trade_time_;
};

}  // namespace lob
