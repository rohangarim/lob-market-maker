#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <random>
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
//   - Optional planted signal (signal_strength in [0,1], default 0 = none): the
//     direction of each reference move is biased by the top-3-level volume
//     imbalance of the book at that moment, P(up) = 0.5 + 0.5 * strength * OBI.
//     This exists ONLY so tests and sweeps can confirm that a strategy using
//     imbalance is wired correctly; it says nothing about real markets. Caveat found
//     by measurement: the book's mid is only a noisy estimate of the hidden reference
//     price, which makes top-3 imbalance correlate NEGATIVELY (about -0.2 at 10 ms)
//     with the next mid change even at strength 0 (mean reversion of that noise), and
//     that swamps the planted effect, so a sweep cannot show imbalance skew paying off.
//   - Optional informed flow (informed_flow in [0,1], default 0): the direction of
//     the next reference move is also biased toward the aggressor side of the most
//     recent trade, P(up) = 0.5 + 0.5 * (strength*OBI + informed_flow*last_trade_sign).
//     With informed_flow = 0 trades carry no information and any passive fill is
//     free spread (positive markout at every horizon, which no real market gives a
//     maker); with it set, fills are followed by moves against the maker, i.e.
//     adverse selection exists, and latency and queue position start to matter.
// Sequence numbers are consecutive; timestamps are non-decreasing.
// ---------------------------------------------------------------------------
class SyntheticBookGenerator {
public:
    static constexpr int kLevelsPerSide = 10;
    static constexpr double kMoveProbability = 0.10;

    SyntheticBookGenerator(uint64_t seed, double start_price, double tick_size, Symbol symbol,
                           double signal_strength = 0.0, double informed_flow = 0.0)
        : rng_(seed),
          ref_(Price::from_double(start_price).ticks()),
          tick_(Price::from_double(tick_size).ticks()),
          symbol_(symbol),
          strength_(signal_strength),
          informed_(informed_flow) {}

    MarketEvent next() {
        t_ += std::chrono::microseconds(std::uniform_int_distribution<int>(50, 2000)(rng_));
        ++seq_;

        if (pending_.empty() && unit() < kMoveProbability) {
            last_move_imbalance_ = imbalance();
            last_move_flow_ = flow_state_;
            bool up = unit() >= 0.5 - 0.5 * (strength_ * last_move_imbalance_ + informed_ * flow_state_);
            flow_state_ = 0.0;
            ref_ += (up ? 1 : -1) * tick_;
            last_move_up_ = up;
            ++moves_;
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

    // Test hooks: the imbalance seen at the most recent reference move and its direction.
    [[nodiscard]] double last_move_imbalance() const noexcept { return last_move_imbalance_; }
    [[nodiscard]] bool last_move_up() const noexcept { return last_move_up_; }
    // Sign (+1 buy / -1 sell / 0 none) of the most recent trade before the last move.
    [[nodiscard]] double last_move_flow() const noexcept { return last_move_flow_; }
    [[nodiscard]] size_t moves() const noexcept { return moves_; }

    [[nodiscard]] size_t bid_levels() const noexcept { return bids_.size(); }
    [[nodiscard]] size_t ask_levels() const noexcept { return asks_.size(); }

private:
    double unit() { return std::uniform_real_distribution<double>(0.0, 1.0)(rng_); }
    Quantity qty() { return std::uniform_int_distribution<Quantity>(1, 500)(rng_); }

    void queue_invalid_levels() {
        const int64_t window = kLevelsPerSide * tick_;
        for (auto it = bids_.begin(); it != bids_.end();) {
            if (it->first >= ref_ || it->first < ref_ - window) {
                pending_.emplace_back(Side::Buy, it->first);
                it = bids_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = asks_.begin(); it != asks_.end();) {
            if (it->first <= ref_ || it->first > ref_ + window) {
                pending_.emplace_back(Side::Sell, it->first);
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
        Quantity q = qty();
        (side == Side::Buy ? bids_ : asks_)[px] = q;
        return MarketEvent::make_quote(t_, seq_, symbol_, side, Price::from_ticks(px), q);
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
            for (size_t i = 0; i < sweep; ++i, ++it) swept.push_back(it->first);
        } else {
            auto it = book_side.rbegin();
            for (size_t i = 0; i < sweep; ++i, ++it) swept.push_back(it->first);
        }
        for (size_t i = 0; i + 1 < swept.size(); ++i) {
            book_side.erase(swept[i]);
            pending_.emplace_back(buy_aggressor ? Side::Sell : Side::Buy, swept[i]);
        }
        flow_state_ = buy_aggressor ? 1.0 : -1.0;
        return MarketEvent::make_trade(t_, seq_, symbol_, buy_aggressor ? Side::Buy : Side::Sell,
                                        Price::from_ticks(swept.back()), qty());
    }

    std::optional<MarketEvent> try_cancel() {
        Side side = unit() < 0.5 ? Side::Buy : Side::Sell;
        auto& levels = side == Side::Buy ? bids_ : asks_;
        if (levels.empty()) return std::nullopt;
        auto it = levels.begin();
        std::advance(it, std::uniform_int_distribution<size_t>(0, levels.size() - 1)(rng_));
        int64_t px = it->first;
        levels.erase(it);
        return MarketEvent::make_delete(t_, seq_, symbol_, side, Price::from_ticks(px));
    }

    // (bid_vol - ask_vol) / (bid_vol + ask_vol) over the top 3 levels per side.
    double imbalance() const {
        Quantity b = 0, a = 0;
        int n = 0;
        for (auto it = bids_.rbegin(); it != bids_.rend() && n < 3; ++it, ++n) b += it->second;
        n = 0;
        for (auto it = asks_.begin(); it != asks_.end() && n < 3; ++it, ++n) a += it->second;
        return (a + b) > 0 ? static_cast<double>(b - a) / static_cast<double>(a + b) : 0.0;
    }

    std::mt19937_64 rng_;
    int64_t ref_;
    int64_t tick_;
    Symbol symbol_;
    SequenceNumber seq_ = 0;
    Timestamp t_{std::chrono::seconds(1'700'000'000)};
    double strength_;
    double informed_;
    double flow_state_ = 0.0;
    double last_move_flow_ = 0.0;
    double last_move_imbalance_ = 0.0;
    bool last_move_up_ = false;
    size_t moves_ = 0;
    std::map<int64_t, Quantity> bids_;
    std::map<int64_t, Quantity> asks_;
    std::deque<std::pair<Side, int64_t>> pending_;
};

}  // namespace lob
