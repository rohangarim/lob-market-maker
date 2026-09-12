#pragma once

#include "common/types.hpp"

namespace lob {

enum class EventType : uint8_t {
    Quote = 0,       // top-of-book or L2 quote update (add/modify a level)
    Trade = 1,       // executed trade print
    BookDelete = 2,  // remove a price level entirely (quantity -> 0)
    Snapshot = 3,    // full-book snapshot marker (used at replay/session start)
    Heartbeat = 4,   // liveness signal from the feed, carries no book state
};

inline std::string_view to_string(EventType t) {
    switch (t) {
        case EventType::Quote: return "QUOTE";
        case EventType::Trade: return "TRADE";
        case EventType::BookDelete: return "BOOK_DELETE";
        case EventType::Snapshot: return "SNAPSHOT";
        case EventType::Heartbeat: return "HEARTBEAT";
    }
    return "UNKNOWN";
}

// ---------------------------------------------------------------------------
// A single, compact, fixed-size market-data event. All external formats
// (provider JSON, historical CSV, binary replay log) are parsed exactly
// once at the system boundary into this struct; every downstream component
// (ring buffer, order book, strategy, recorder) operates on MarketEvent
// only and never re-parses a wire format. This keeps the hot path free of
// string parsing/allocation and gives the ring buffer/object pool a
// trivially-copyable, fixed-size payload.
// ---------------------------------------------------------------------------
struct MarketEvent {
    Timestamp timestamp{};
    SequenceNumber sequence = 0;
    Symbol symbol{};
    EventType type = EventType::Heartbeat;
    Price price{};
    Quantity quantity = 0;
    Side side = Side::Buy;

    static MarketEvent make_quote(Timestamp ts, SequenceNumber seq, Symbol sym,
                                   Side side, Price px, Quantity qty) {
        MarketEvent e;
        e.timestamp = ts;
        e.sequence = seq;
        e.symbol = sym;
        e.type = EventType::Quote;
        e.price = px;
        e.quantity = qty;
        e.side = side;
        return e;
    }

    static MarketEvent make_trade(Timestamp ts, SequenceNumber seq, Symbol sym,
                                   Side aggressor_side, Price px, Quantity qty) {
        MarketEvent e;
        e.timestamp = ts;
        e.sequence = seq;
        e.symbol = sym;
        e.type = EventType::Trade;
        e.price = px;
        e.quantity = qty;
        e.side = aggressor_side;
        return e;
    }

    static MarketEvent make_delete(Timestamp ts, SequenceNumber seq, Symbol sym,
                                    Side side, Price px) {
        MarketEvent e;
        e.timestamp = ts;
        e.sequence = seq;
        e.symbol = sym;
        e.type = EventType::BookDelete;
        e.price = px;
        e.quantity = 0;
        e.side = side;
        return e;
    }

    static MarketEvent make_heartbeat(Timestamp ts, SequenceNumber seq, Symbol sym) {
        MarketEvent e;
        e.timestamp = ts;
        e.sequence = seq;
        e.symbol = sym;
        e.type = EventType::Heartbeat;
        return e;
    }
};

static_assert(std::is_trivially_copyable_v<MarketEvent>,
              "MarketEvent must stay trivially copyable for ring-buffer/pool use");

}  // namespace lob
