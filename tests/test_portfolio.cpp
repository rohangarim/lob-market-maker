#include <gtest/gtest.h>

#include "portfolio/portfolio.hpp"

using namespace lob;

TEST(Portfolio, StartsFlat) {
    Portfolio p;
    EXPECT_EQ(p.state().position, 0);
    EXPECT_DOUBLE_EQ(p.state().realized_pnl, 0.0);
}

TEST(Portfolio, SingleBuyOpensLongPositionAtFillPrice) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    EXPECT_EQ(p.state().position, 10);
    EXPECT_DOUBLE_EQ(p.state().avg_entry_price, 100.00);
    EXPECT_EQ(p.state().total_volume_traded, 10);
}

TEST(Portfolio, ExtendingLongUpdatesWeightedAverageEntryPrice) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.on_fill(Side::Buy, Price::from_double(110.00), 10);
    EXPECT_EQ(p.state().position, 20);
    EXPECT_NEAR(p.state().avg_entry_price, 105.00, 1e-9);
}

TEST(Portfolio, ClosingPartiallyRealizesProportionalPnl) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.on_fill(Side::Sell, Price::from_double(105.00), 4);
    EXPECT_EQ(p.state().position, 6);
    EXPECT_NEAR(p.state().realized_pnl, 4 * (105.00 - 100.00), 1e-9);
    EXPECT_NEAR(p.state().avg_entry_price, 100.00, 1e-9);  // unchanged for remaining shares
}

TEST(Portfolio, FlippingFromLongToShortResetsAvgEntryOnExcess) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.on_fill(Side::Sell, Price::from_double(105.00), 15);  // closes 10 long, opens 5 short
    EXPECT_EQ(p.state().position, -5);
    EXPECT_NEAR(p.state().realized_pnl, 10 * (105.00 - 100.00), 1e-9);
    EXPECT_NEAR(p.state().avg_entry_price, 105.00, 1e-9);
}

TEST(Portfolio, ShortPositionProfitsWhenPriceFalls) {
    Portfolio p;
    p.on_fill(Side::Sell, Price::from_double(100.00), 10);
    EXPECT_EQ(p.state().position, -10);
    p.on_fill(Side::Buy, Price::from_double(90.00), 10);  // cover short at a lower price
    EXPECT_EQ(p.state().position, 0);
    EXPECT_NEAR(p.state().realized_pnl, 10 * (100.00 - 90.00), 1e-9);
}

TEST(Portfolio, MarkToMarketUpdatesUnrealizedAndTotalPnl) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.mark_to_market(102.00);
    EXPECT_NEAR(p.state().unrealized_pnl, 20.00, 1e-9);
    EXPECT_NEAR(p.state().total_pnl, p.state().realized_pnl + p.state().unrealized_pnl, 1e-9);
}

TEST(Portfolio, MarkToMarketOnShortPositionSignsCorrectly) {
    Portfolio p;
    p.on_fill(Side::Sell, Price::from_double(100.00), 10);
    p.mark_to_market(95.00);  // price fell -> short is profitable
    EXPECT_NEAR(p.state().unrealized_pnl, 50.00, 1e-9);
    p.mark_to_market(105.00);  // price rose -> short is now losing
    EXPECT_NEAR(p.state().unrealized_pnl, -50.00, 1e-9);
}

TEST(Portfolio, TurnoverAndVolumeAccumulateAcrossFills) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.on_fill(Side::Sell, Price::from_double(101.00), 5);
    EXPECT_EQ(p.state().total_volume_traded, 15);
    EXPECT_NEAR(p.state().turnover, 10 * 100.00 + 5 * 101.00, 1e-9);
}

TEST(Portfolio, HistoryRecordsOneEntryPerFill) {
    Portfolio p;
    p.on_fill(Side::Buy, Price::from_double(100.00), 10);
    p.on_fill(Side::Buy, Price::from_double(101.00), 10);
    p.on_fill(Side::Sell, Price::from_double(102.00), 5);
    EXPECT_EQ(p.history().size(), 3u);
}
