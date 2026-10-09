#include <gtest/gtest.h>

#include "marketdata/trade_tape_adapter.hpp"
#include "orderbook/order_book.hpp"

using namespace lob;

namespace {
FinnhubTrade trade(double px, double vol, int64_t ms = 1000) { return {"AAPL", px, vol, ms}; }
}  // namespace

TEST(TradeTapeAdapter, FirstTradeEmitsTradePlusTwoSyntheticQuotes) {
    TradeTapeAdapter a(0.02);
    std::vector<MarketEvent> out;
    a.on_trade(trade(100.00, 50), out);
    ASSERT_EQ(out.size(), 3u);
    EXPECT_EQ(out[0].type, EventType::Trade);
    EXPECT_EQ(out[1].type, EventType::Quote);
    EXPECT_EQ(out[2].type, EventType::Quote);
    EXPECT_DOUBLE_EQ(out[1].price.to_double(), 99.99);
    EXPECT_DOUBLE_EQ(out[2].price.to_double(), 100.01);
    EXPECT_EQ(out[0].timestamp, std::chrono::milliseconds(1000));
}

TEST(TradeTapeAdapter, TickRuleClassifiesAggressorSide) {
    TradeTapeAdapter a(0.02);
    std::vector<MarketEvent> out;
    a.on_trade(trade(100.00, 1), out);
    a.on_trade(trade(100.50, 1), out);  // uptick
    a.on_trade(trade(100.25, 1), out);  // downtick
    a.on_trade(trade(100.25, 1), out);  // unchanged -> repeats previous side
    std::vector<Side> sides;
    for (auto& e : out) if (e.type == EventType::Trade) sides.push_back(e.side);
    ASSERT_EQ(sides.size(), 4u);
    EXPECT_EQ(sides[1], Side::Buy);
    EXPECT_EQ(sides[2], Side::Sell);
    EXPECT_EQ(sides[3], Side::Sell);
}

TEST(TradeTapeAdapter, OverlayKeepsExactlyOneLevelPerSideAndNeverCrosses) {
    TradeTapeAdapter a(0.02);
    OrderBook book(Symbol("AAPL"));
    std::vector<MarketEvent> out;
    const double prices[] = {100.00, 100.01, 99.90, 105.00, 104.00, 104.00, 90.00};
    for (double px : prices) {
        out.clear();
        a.on_trade(trade(px, 10), out);
        for (auto& e : out) {
            book.apply(e);
            ASSERT_FALSE(book.crossed()) << "crossed after event type " << static_cast<int>(e.type);
        }
        EXPECT_EQ(book.bid_level_count(), 1u);
        EXPECT_EQ(book.ask_level_count(), 1u);
        EXPECT_NEAR(*book.mid_price(), px, 1e-9);
    }
}

TEST(TradeTapeAdapter, FractionalAndZeroVolumeBecomeQuantityOne) {
    TradeTapeAdapter a(0.02);
    std::vector<MarketEvent> out;
    a.on_trade(trade(100.0, 0.2), out);
    a.on_trade(trade(100.0, 0.0), out);
    for (auto& e : out) EXPECT_GE(e.quantity, 1) << "a zero quantity would delete the level";
}

TEST(TradeTapeAdapter, SequenceNumbersAreConsecutive) {
    TradeTapeAdapter a(0.02);
    std::vector<MarketEvent> out;
    for (double px : {100.0, 101.0, 100.5}) a.on_trade(trade(px, 5), out);
    for (size_t i = 0; i < out.size(); ++i) EXPECT_EQ(out[i].sequence, i + 1);
}

TEST(TradeTapeAdapter, SamePriceDoesNotEmitDeletes) {
    TradeTapeAdapter a(0.02);
    std::vector<MarketEvent> out;
    a.on_trade(trade(100.0, 5), out);
    out.clear();
    a.on_trade(trade(100.0, 5), out);
    EXPECT_EQ(out.size(), 3u);  // trade + 2 quote refreshes, no deletes
}
