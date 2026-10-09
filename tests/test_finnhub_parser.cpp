#include <gtest/gtest.h>

#include "marketdata/finnhub_parser.hpp"

using namespace lob;

// Payloads below follow Finnhub's documented trade-message shape. They are
// canned, not captured from a live session.

TEST(FinnhubParser, ParsesDocumentedTradeMessage) {
    auto r = parse_finnhub_message(
        R"({"data":[{"c":["1","8"],"p":190.12,"s":"AAPL","t":1700000000123,"v":100}],"type":"trade"})");
    ASSERT_EQ(r.status, FinnhubParseStatus::Trades);
    ASSERT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.trades[0].symbol, "AAPL");
    EXPECT_DOUBLE_EQ(r.trades[0].price, 190.12);
    EXPECT_DOUBLE_EQ(r.trades[0].volume, 100.0);
    EXPECT_EQ(r.trades[0].timestamp_ms, 1700000000123);
}

TEST(FinnhubParser, ParsesMultipleTicksInOneMessage) {
    auto r = parse_finnhub_message(
        R"({"data":[{"p":1.5,"s":"A","t":1,"v":2},{"p":2.5,"s":"B","t":2,"v":3}],"type":"trade"})");
    ASSERT_EQ(r.status, FinnhubParseStatus::Trades);
    EXPECT_EQ(r.trades.size(), 2u);
    EXPECT_EQ(r.malformed_ticks, 0u);
}

TEST(FinnhubParser, PingIsIgnoredNotMalformed) {
    EXPECT_EQ(parse_finnhub_message(R"({"type":"ping"})").status, FinnhubParseStatus::Ignored);
}

TEST(FinnhubParser, ErrorMessageIsIgnored) {
    EXPECT_EQ(parse_finnhub_message(R"({"type":"error","msg":"Subscribing to too many symbols"})").status,
              FinnhubParseStatus::Ignored);
}

TEST(FinnhubParser, NonJsonIsMalformed) {
    EXPECT_EQ(parse_finnhub_message("not json at all").status, FinnhubParseStatus::Malformed);
    EXPECT_EQ(parse_finnhub_message("").status, FinnhubParseStatus::Malformed);
    EXPECT_EQ(parse_finnhub_message("{\"type\":\"trade\",").status, FinnhubParseStatus::Malformed);
}

TEST(FinnhubParser, NonObjectJsonIsMalformed) {
    EXPECT_EQ(parse_finnhub_message("[1,2,3]").status, FinnhubParseStatus::Malformed);
    EXPECT_EQ(parse_finnhub_message("42").status, FinnhubParseStatus::Malformed);
}

TEST(FinnhubParser, MissingOrMistypedTypeFieldIsMalformed) {
    EXPECT_EQ(parse_finnhub_message(R"({"data":[]})").status, FinnhubParseStatus::Malformed);
    EXPECT_EQ(parse_finnhub_message(R"({"type":7})").status, FinnhubParseStatus::Malformed);
}

TEST(FinnhubParser, TradeWithoutDataArrayIsMalformed) {
    EXPECT_EQ(parse_finnhub_message(R"({"type":"trade"})").status, FinnhubParseStatus::Malformed);
    EXPECT_EQ(parse_finnhub_message(R"({"type":"trade","data":5})").status, FinnhubParseStatus::Malformed);
}

TEST(FinnhubParser, BadTicksAreSkippedAndCountedWithoutDroppingGoodOnes) {
    auto r = parse_finnhub_message(R"({"type":"trade","data":[
        {"p":"190.1","s":"AAPL","t":1,"v":1},
        {"p":190.1,"s":"AAPL","t":1},
        {"p":-5,"s":"AAPL","t":1,"v":1},
        {"p":190.1,"s":"AAPL","t":1.5,"v":1},
        5,
        {"p":190.2,"s":"AAPL","t":9,"v":4}]})");
    ASSERT_EQ(r.status, FinnhubParseStatus::Trades);
    EXPECT_EQ(r.trades.size(), 1u);
    EXPECT_EQ(r.malformed_ticks, 5u);
    EXPECT_DOUBLE_EQ(r.trades[0].price, 190.2);
}

TEST(FinnhubParser, EmptyDataArrayIsAValidEmptyTradeMessage) {
    auto r = parse_finnhub_message(R"({"type":"trade","data":[]})");
    EXPECT_EQ(r.status, FinnhubParseStatus::Trades);
    EXPECT_TRUE(r.trades.empty());
}
