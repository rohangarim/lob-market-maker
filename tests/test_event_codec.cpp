#include <gtest/gtest.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <unistd.h>

#include "replay/event_codec.hpp"

using namespace lob;

namespace {
// mkstemp-based temp file path, avoiding the deprecated/unsafe tmpnam().
std::string unique_temp_path() {
    std::string tmpl = "/tmp/lob_event_codec_test_XXXXXX";
    int fd = mkstemp(tmpl.data());
    if (fd != -1) close(fd);
    return tmpl;
}
}  // namespace

TEST(EventCodec, RoundTripSingleEventThroughStream) {
    MarketEvent original = MarketEvent::make_quote(Timestamp(123456789), 42, Symbol("AAPL"),
                                                     Side::Sell, Price::from_double(101.2345), 77);
    std::stringstream ss;
    write_event(ss, original);
    ss.seekg(0);
    auto decoded = read_event(ss);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->timestamp, original.timestamp);
    EXPECT_EQ(decoded->sequence, original.sequence);
    EXPECT_EQ(decoded->symbol.view(), original.symbol.view());
    EXPECT_EQ(decoded->type, original.type);
    EXPECT_EQ(decoded->price.ticks(), original.price.ticks());
    EXPECT_EQ(decoded->quantity, original.quantity);
    EXPECT_EQ(decoded->side, original.side);
}

TEST(EventCodec, RoundTripFileWithMultipleEvents) {
    std::vector<MarketEvent> events;
    for (int i = 0; i < 500; ++i) {
        events.push_back(MarketEvent::make_trade(Timestamp(i), static_cast<SequenceNumber>(i),
                                                   Symbol("MSFT"), i % 2 == 0 ? Side::Buy : Side::Sell,
                                                   Price::from_double(300.0 + i * 0.01), i + 1));
    }
    std::string path = unique_temp_path();
    write_events_to_file(path, events);
    auto loaded = read_events_from_file(path);
    std::remove(path.c_str());

    ASSERT_EQ(loaded.size(), events.size());
    for (size_t i = 0; i < events.size(); ++i) {
        EXPECT_EQ(loaded[i].sequence, events[i].sequence);
        EXPECT_EQ(loaded[i].price.ticks(), events[i].price.ticks());
        EXPECT_EQ(loaded[i].quantity, events[i].quantity);
    }
}

TEST(EventCodec, RejectsFileWithoutMagicHeader) {
    std::string path = unique_temp_path();
    {
        std::ofstream ofs(path, std::ios::binary);
        ofs << "not a valid event log";
    }
    EXPECT_THROW(read_events_from_file(path), std::runtime_error);
    std::remove(path.c_str());
}

TEST(EventCodec, TruncatedRecordAtEndOfFileIsIgnoredNotCrashed) {
    std::string path = unique_temp_path();
    {
        std::ofstream ofs(path, std::ios::binary);
        write_magic(ofs);
        write_event(ofs, MarketEvent::make_trade(Timestamp(1), 1, Symbol("AAPL"), Side::Buy,
                                                   Price::from_double(100.0), 10));
        ofs.write("\x01\x02\x03", 3);  // partial/garbage trailing record
    }
    auto events = read_events_from_file(path);
    EXPECT_EQ(events.size(), 1u);
    std::remove(path.c_str());
}

TEST(EventCodec, SymbolLongerThanMaxIsTruncatedNotCorrupted) {
    Symbol s("ABCDEFGHIJKLMNOPQRSTUVWXYZ");  // longer than kMaxLen
    MarketEvent ev = MarketEvent::make_heartbeat(Timestamp(1), 1, s);
    std::stringstream ss;
    write_event(ss, ev);
    ss.seekg(0);
    auto decoded = read_event(ss);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(decoded->symbol.view().size(), Symbol::kMaxLen);
}
