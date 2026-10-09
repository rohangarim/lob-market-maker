#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <optional>
#include <random>
#include <set>
#include <vector>

#include "marketdata/events.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Seeded synthetic L2 event generator. NOT real market data.
//
// The generator tracks the book it is implicitly describing so the emitted
// stream stays internally consistent:
//   - A reference price lives on the tick grid and moves +/- one tick with
//     probability kMoveProbability per event.
//   - Resting bids always sit in [ref - K*tick, ref - tick] and asks in
//     [ref + tick, ref + K*tick]. When the reference moves, every tracked
//     level that violates this is queued as a BookDelete (one emitted per
//     event, ahead of any new activity), so stale levels never accumulate and
//     the book is not left crossed (a brief transient is possible if two
//     moves land before the delete queue drains).
//   - A trade is an aggressor sweeping k >= 1 levels of the opposite side
//     (k geometric). It prints at the deepest swept level's price, and the
//     fully-consumed levels are queued for deletion, as an exchange would.
//   - Remaining events are quote upserts biased toward the touch, random
//     cancels of existing levels, and heartbeats.
// Sequence numbers are consecutive; timestamps are non-decreasing.
// ---------------------------------------------------------------------------
class SyntheticBookGenerator {
public:
    static constexpr int kLevelsPerSide = 10;
    static constexpr double kMoveProbability = 0.10;

    SyntheticBookGenerator(uint64_t seed, double start_price, double tick_size, Symbol symbol)
        : rng_(seed),
          ref_(Price::from_double(start_price).ticks()),
          tick_(Price::from_double(tick_size).ticks()),
          symbol_(symbol) {}

    MarketEvent next() {
        t_ += std::chrono::microseconds(std::uniform_int_distribution<int>(50, 2000)(rng_));
        ++seq_;

        if (pending_.empty() && unit() < kMoveProbability) {
            ref_ += (unit() < 0.5 ? -1 : 1) * tick_;
            queue_invalid_levels();
        }
        if (!pending_.empty()) {
            auto [side, px] = pending_.front();
            pending_.pop_front();
            return MarketEvent::make_delete(t_, seq_, symbol_, side, Price::from_ticks(px));
        }

        double roll = unit();
        if (roll < 0.05) {
            return MarketEvent::make_heartbeat(t_, seq_, symbol_);
        }
        if (roll < 0.30) {
            if (auto trade = try_trade()) return *trade;
        } else if (roll < 0.38) {
            if (auto cancel = try_cancel()) return *cancel;
        }
        return make_quote();
    }

    [[nodiscard]] size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_levels() const noexcept { return asks_.size(); }

private:
    double unit() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    Quantity qty() { return std::uniform_int_distribution<Quantity>(1, 500)(rng_); }

    void queue_invalid_levels() {
        const int64_t window = kLevelsPerSide * tick_;
        for (auto it = bids_.begin(); it != bids_.end();) {
            if (*it >= ref_ || *it < ref_ - window) {
                pending_.emplace_back(Side::Buy, *it);
                it = bids_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = asks_.begin(); it != asks_.end();) {
            if (*it <= ref_ || *it > ref_ + window) {
                pending_.emplace_back(Side::Sell, *it);
                it = asks_.erase(it);
            } else {
                ++it;
            }
        }
    }

    MarketEvent make_quote() {
        Side side = unit() < 0.5 ? Side::Buy : Side::Sell;
        int level = static_cast<int>(std::pow(unit(), 2.0) * kLevelsPerSide);
        level = std::min(level, kLevelsPerSide - 1);
        int64_t px = side == Side::Buy ? ref_ - tick_ * (level + 1) : ref_ + tick_ * (level + 1);
        (side == Side::Buy ? bids_ : asks_).insert(px);
        return MarketEvent::make_quote(t_, seq_, symbol_, side, Price::from_ticks(px), qty());
    }

    std::optional<MarketEvent> try_trade() {
        bool buy_aggressor = unit() < 0.5;
        auto& book_side = buy_aggressor ? asks_ : bids_;
        if (book_side.empty()) return std::nullopt;

        size_t sweep = 1;
        while (sweep < book_side.size() && unit() < 0.5) ++sweep;

        std::vector<int64_t> swept;
        if (buy_aggressor) {
            auto it = book_side.begin();
            for (size_t i = 0; i < sweep; ++i, ++it) swept.push_back(*it);
        } else {
            auto it = book_side.rbegin();
            for (size_t i = 0; i < sweep; ++i, ++it) swept.push_back(*it);
        }
        for (size_t i = 0; i + 1 < swept.size(); ++i) {
            book_side.erase(swept[i]);
            pending_.emplace_back(buy_aggressor ? Side::Sell : Side::Buy, swept[i]);
        }
        return MarketEvent::make_trade(t_, seq_, symbol_, buy_aggressor ? Side::Buy : Side::Sell,
                                        Price::from_ticks(swept.back()), qty());
    }

    std::optional<MarketEvent> try_cancel() {
        Side side = unit() < 0.5 ? Side::Buy : Side::Sell;
        auto& levels = side == Side::Buy ? bids_ : asks_;
        if (levels.empty()) return std::nullopt;
        auto it = levels.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, levels.size() - 1)(rng_));
        int64_t px = *it;
        levels.erase(it);
        return MarketEvent::make_delete(t_, seq_, symbol_, side, Price::from_ticks(px));
    }

    std::mt19937_64 rng_;
    int64_t ref_;
    int64_t tick_;
    Symbol symbol_;
    SequenceNumber seq_ = 0;
    Timestamp t_{std::chrono::seconds(1'700'000'000)};
    std::set<int64_t> bids_;
    std::set<int64_t> asks_;
    std::deque<std::pair<Side, int64_t>> pending_;
};

}  // namespace lob
