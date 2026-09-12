#include "replay/event_codec.hpp"

#include <cstring>
#include <fstream>
#include <stdexcept>

namespace lob {

namespace {

void put_u64(std::ostream& os, uint64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void put_i64(std::ostream& os, int64_t v) { os.write(reinterpret_cast<const char*>(&v), 8); }
void put_u8(std::ostream& os, uint8_t v) { os.write(reinterpret_cast<const char*>(&v), 1); }

bool get_u64(std::istream& is, uint64_t& v) {
    is.read(reinterpret_cast<char*>(&v), 8);
    return static_cast<bool>(is);
}
bool get_i64(std::istream& is, int64_t& v) {
    is.read(reinterpret_cast<char*>(&v), 8);
    return static_cast<bool>(is);
}
bool get_u8(std::istream& is, uint8_t& v) {
    is.read(reinterpret_cast<char*>(&v), 1);
    return static_cast<bool>(is);
}

}  // namespace

void write_magic(std::ostream& os) {
    os.write(kEventLogMagic, sizeof(kEventLogMagic));
}

bool read_and_check_magic(std::istream& is) {
    char buf[sizeof(kEventLogMagic)];
    is.read(buf, sizeof(buf));
    if (!is) return false;
    return std::memcmp(buf, kEventLogMagic, sizeof(kEventLogMagic)) == 0;
}

void write_event(std::ostream& os, const MarketEvent& ev) {
    put_u64(os, static_cast<uint64_t>(ev.timestamp.count()));
    put_u64(os, ev.sequence);

    std::string_view sym = ev.symbol.view();
    uint8_t len = static_cast<uint8_t>(std::min<size_t>(sym.size(), 15));
    put_u8(os, len);
    char padded[15] = {0};
    std::memcpy(padded, sym.data(), len);
    os.write(padded, 15);

    put_u8(os, static_cast<uint8_t>(ev.type));
    put_i64(os, ev.price.ticks());
    put_i64(os, static_cast<int64_t>(ev.quantity));
    put_u8(os, static_cast<uint8_t>(ev.side));
}

std::optional<MarketEvent> read_event(std::istream& is) {
    uint64_t ts_ns = 0, seq = 0;
    if (!get_u64(is, ts_ns)) return std::nullopt;
    if (!get_u64(is, seq)) return std::nullopt;

    uint8_t len = 0;
    if (!get_u8(is, len)) return std::nullopt;
    char padded[15];
    is.read(padded, 15);
    if (!is) return std::nullopt;

    uint8_t type_raw = 0;
    if (!get_u8(is, type_raw)) return std::nullopt;
    int64_t price_ticks = 0;
    if (!get_i64(is, price_ticks)) return std::nullopt;
    int64_t qty = 0;
    if (!get_i64(is, qty)) return std::nullopt;
    uint8_t side_raw = 0;
    if (!get_u8(is, side_raw)) return std::nullopt;

    MarketEvent ev;
    ev.timestamp = Timestamp(static_cast<int64_t>(ts_ns));
    ev.sequence = seq;
    ev.symbol = Symbol(std::string_view(padded, len));
    ev.type = static_cast<EventType>(type_raw);
    ev.price = Price::from_ticks(price_ticks);
    ev.quantity = qty;
    ev.side = static_cast<Side>(side_raw);
    return ev;
}

void write_events_to_file(const std::string& path, const std::vector<MarketEvent>& events) {
    std::ofstream ofs(path, std::ios::binary | std::ios::trunc);
    if (!ofs) throw std::runtime_error("failed to open event log for writing: " + path);
    write_magic(ofs);
    for (const auto& ev : events) write_event(ofs, ev);
}

std::vector<MarketEvent> read_events_from_file(const std::string& path) {
    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) throw std::runtime_error("failed to open event log for reading: " + path);
    if (!read_and_check_magic(ifs)) {
        throw std::runtime_error("invalid or missing magic header in event log: " + path);
    }
    std::vector<MarketEvent> events;
    while (auto ev = read_event(ifs)) {
        events.push_back(*ev);
    }
    return events;
}

}  // namespace lob
