#pragma once

#include <cstdint>
#include <iosfwd>
#include <optional>
#include <vector>

#include "marketdata/events.hpp"

namespace lob {

// ---------------------------------------------------------------------------
// Compact fixed-size binary event log format.
//
// File layout: an 8-byte magic/version header ("LOBEVT01") followed by a
// sequence of fixed-size 50-byte records, one per MarketEvent. Fields are
// serialized individually in little-endian byte order (this machine and
// every realistic deployment target are little-endian; a portable
// byte-swap could be added if that ever changes) rather than memcpy'ing the
// in-memory MarketEvent struct, so the on-disk format is stable regardless
// of struct padding/alignment changes in the C++ type.
//
// Record layout:
//   u64 timestamp_ns
//   u64 sequence
//   u8  symbol_len
//   char symbol[15]   (padded with zero bytes past symbol_len)
//   u8  event_type
//   i64 price_ticks
//   i64 quantity
//   u8  side
// = 8 + 8 + 1 + 15 + 1 + 8 + 8 + 1 = 50 bytes
// ---------------------------------------------------------------------------
inline constexpr char kEventLogMagic[8] = {'L', 'O', 'B', 'E', 'V', 'T', '0', '1'};
inline constexpr size_t kRecordSize = 50;

void write_magic(std::ostream& os);
bool read_and_check_magic(std::istream& is);

void write_event(std::ostream& os, const MarketEvent& ev);
std::optional<MarketEvent> read_event(std::istream& is);

// Convenience whole-file helpers (loads everything into memory; fine for
// the dataset sizes this project's replay/benchmark tooling uses).
void write_events_to_file(const std::string& path, const std::vector<MarketEvent>& events);
std::vector<MarketEvent> read_events_from_file(const std::string& path);

}  // namespace lob
