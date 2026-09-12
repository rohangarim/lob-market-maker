#pragma once

#include <cstdint>
#include <chrono>
#include <string>
#include <string_view>
#include <array>
#include <compare>
#include <ostream>

namespace lob {

// ---------------------------------------------------------------------------
// Price is represented as an integer number of "ticks" rather than a double.
// Ticks avoid floating point rounding error accumulating across incremental
// book updates and make price comparisons/hashing exact and branch-free.
// kPriceScale ticks == $1.00. 4 decimal places covers sub-penny quotes.
// ---------------------------------------------------------------------------
class Price {
public:
    static constexpr int64_t kScale = 10'000;

    constexpr Price() noexcept = default;
    static constexpr Price from_ticks(int64_t ticks) noexcept {
        Price p;
        p.ticks_ = ticks;
        return p;
    }
    static constexpr Price from_double(double dollars) noexcept {
        return from_ticks(static_cast<int64_t>(dollars * static_cast<double>(kScale) + (dollars >= 0 ? 0.5 : -0.5)));
    }

    [[nodiscard]] constexpr int64_t ticks() const noexcept { return ticks_; }
    [[nodiscard]] constexpr double to_double() const noexcept {
        return static_cast<double>(ticks_) / static_cast<double>(kScale);
    }

    friend constexpr auto operator<=>(Price, Price) noexcept = default;

    constexpr Price operator+(Price other) const noexcept { return from_ticks(ticks_ + other.ticks_); }
    constexpr Price operator-(Price other) const noexcept { return from_ticks(ticks_ - other.ticks_); }

private:
    int64_t ticks_ = 0;
};

inline std::ostream& operator<<(std::ostream& os, Price p) {
    return os << p.to_double();
}

using Quantity = int64_t;
using OrderId = uint64_t;
using SequenceNumber = uint64_t;

// Nanoseconds since Unix epoch. Sourced from exchange/provider timestamps
// when available, otherwise from the local steady/system clock at ingestion.
using Timestamp = std::chrono::nanoseconds;

inline Timestamp now_ns() noexcept {
    return std::chrono::duration_cast<Timestamp>(
        std::chrono::system_clock::now().time_since_epoch());
}

enum class Side : uint8_t { Buy = 0, Sell = 1 };

constexpr Side opposite(Side s) noexcept {
    return s == Side::Buy ? Side::Sell : Side::Buy;
}

inline std::ostream& operator<<(std::ostream& os, Side s) {
    return os << (s == Side::Buy ? "BUY" : "SELL");
}

// Fixed-capacity symbol string (no heap allocation, cheap to copy, fits in
// a cache line alongside other event fields).
class Symbol {
public:
    static constexpr size_t kMaxLen = 15;

    constexpr Symbol() noexcept = default;
    Symbol(std::string_view sv) noexcept {
        size_t n = sv.size() < kMaxLen ? sv.size() : kMaxLen;
        for (size_t i = 0; i < n; ++i) data_[i] = sv[i];
        len_ = static_cast<uint8_t>(n);
    }

    [[nodiscard]] std::string_view view() const noexcept {
        return std::string_view(data_.data(), len_);
    }
    [[nodiscard]] std::string str() const { return std::string(view()); }

    friend bool operator==(const Symbol& a, const Symbol& b) noexcept {
        return a.view() == b.view();
    }

private:
    std::array<char, kMaxLen> data_{};
    uint8_t len_ = 0;
};

inline std::ostream& operator<<(std::ostream& os, const Symbol& s) {
    return os << s.view();
}

enum class OrderType : uint8_t { Limit = 0, Market = 1 };
enum class TimeInForce : uint8_t { GTC = 0, IOC = 1, FOK = 2 };

enum class OrderStatus : uint8_t {
    New = 0,
    PartiallyFilled = 1,
    Filled = 2,
    Canceled = 3,
    Rejected = 4
};

inline std::string_view to_string(OrderStatus s) {
    switch (s) {
        case OrderStatus::New: return "NEW";
        case OrderStatus::PartiallyFilled: return "PARTIALLY_FILLED";
        case OrderStatus::Filled: return "FILLED";
        case OrderStatus::Canceled: return "CANCELED";
        case OrderStatus::Rejected: return "REJECTED";
    }
    return "UNKNOWN";
}

}  // namespace lob
