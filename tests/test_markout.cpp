#include <gtest/gtest.h>

#include "metrics/markout.hpp"

using namespace lob;

namespace {
Timestamp ms(int64_t v) { return std::chrono::milliseconds(v); }
}  // namespace

TEST(MarkoutTracker, BuyFillEdgeAndMarkoutPerHorizon) {
    MarkoutTracker m({ms(1), ms(10)});
    m.on_fill(Side::Buy, 100.00, 10, /*mid_at_fill=*/100.05, ms(0));
    EXPECT_NEAR(m.avg_edge(), 0.05, 1e-12);

    m.on_time(ms(1), 100.03);   // due for horizon 0
    EXPECT_NEAR(m.avg_markout(0), 0.03, 1e-12);
    EXPECT_DOUBLE_EQ(m.marked_qty(1), 0.0);  // horizon 1 not yet due

    m.on_time(ms(10), 99.90);
    EXPECT_NEAR(m.avg_markout(1), -0.10, 1e-12);
    EXPECT_NEAR(m.avg_markout(0), 0.03, 1e-12);  // already-marked horizon unchanged
}

TEST(MarkoutTracker, SellFillHasMirroredSigns) {
    MarkoutTracker m({ms(1)});
    m.on_fill(Side::Sell, 100.10, 5, 100.05, ms(0));
    EXPECT_NEAR(m.avg_edge(), 0.05, 1e-12);        // sold above mid
    m.on_time(ms(1), 100.20);                      // price rose after we sold: adverse
    EXPECT_NEAR(m.avg_markout(0), -0.10, 1e-12);
}

TEST(MarkoutTracker, AveragesAreVolumeWeighted) {
    MarkoutTracker m({ms(1)});
    m.on_fill(Side::Buy, 100.00, 10, 100.00, ms(0));   // markout +0.10 per share
    m.on_fill(Side::Buy, 100.00, 30, 100.00, ms(0));   // markout -0.10 per share
    m.on_time(ms(1), 100.10);
    EXPECT_NEAR(m.avg_markout(0), 0.10, 1e-12);        // both marked at the same mid...
    MarkoutTracker n({ms(1)});
    n.on_fill(Side::Buy, 100.00, 10, 100.00, ms(0));
    n.on_time(ms(1), 100.10);                          // +0.10 on 10 shares
    n.on_fill(Side::Buy, 100.00, 30, 100.00, ms(1));
    n.on_time(ms(2), 99.90);                           // -0.10 on 30 shares
    EXPECT_NEAR(n.avg_markout(0), (0.10 * 10 - 0.10 * 30) / 40.0, 1e-12);
}

TEST(MarkoutTracker, FillsNotYetDueAreExcludedNotCountedAsZero) {
    MarkoutTracker m({ms(100)});
    m.on_fill(Side::Buy, 100.00, 10, 100.00, ms(0));
    m.on_time(ms(50), 101.0);
    EXPECT_DOUBLE_EQ(m.marked_qty(0), 0.0);
    EXPECT_DOUBLE_EQ(m.avg_markout(0), 0.0);
    EXPECT_EQ(m.fill_count(), 1u);
}

TEST(MarkoutTracker, UsesTheMidInForceBeforeTheDueEvent) {
    // The caller passes the pre-event mid; verify it is what gets used.
    MarkoutTracker m({ms(1)});
    m.on_fill(Side::Buy, 100.00, 1, 100.00, ms(0));
    m.on_time(ms(5), 100.40);   // event at 5ms, previous mid 100.40
    EXPECT_NEAR(m.avg_markout(0), 0.40, 1e-12);
}

TEST(MarkoutTracker, IgnoresZeroQuantityAndExtraHorizons) {
    MarkoutTracker m({ms(1), ms(2), ms(3), ms(4), ms(5), ms(6), ms(7), ms(8)});
    EXPECT_EQ(m.horizon_count(), MarkoutTracker::kMaxHorizons);
    m.on_fill(Side::Buy, 100.0, 0, 100.0, ms(0));
    EXPECT_EQ(m.fill_count(), 0u);
}

TEST(MarkoutTracker, NoHorizonsStillTracksEdge) {
    MarkoutTracker m;
    m.on_fill(Side::Buy, 99.99, 10, 100.00, ms(0));
    EXPECT_NEAR(m.avg_edge(), 0.01, 1e-12);
    m.on_time(ms(1000), 100.0);  // must not crash
}

TEST(RunAnalytics, TracksPeakDrawdownAndPosition) {
    RunAnalytics a;
    a.sample(5, 10.0);
    a.sample(-8, 25.0);
    a.sample(0, 15.0);   // 10 below the 25 peak
    a.sample(3, 20.0);
    a.sample(2, 5.0);    // 20 below the peak
    EXPECT_EQ(a.max_abs_position, 8);
    EXPECT_DOUBLE_EQ(a.max_drawdown, 20.0);
    EXPECT_DOUBLE_EQ(a.peak_pnl, 25.0);
    EXPECT_DOUBLE_EQ(a.mean_abs_position(), (5 + 8 + 0 + 3 + 2) / 5.0);
}

TEST(RunAnalytics, MonotoneGainsHaveNoDrawdown) {
    RunAnalytics a;
    for (int i = 0; i < 10; ++i) a.sample(1, i);
    EXPECT_DOUBLE_EQ(a.max_drawdown, 0.0);
}

TEST(RunAnalytics, DrawdownCanBeMeasuredFromAnInitialLoss) {
    RunAnalytics a;
    a.sample(0, -5.0);
    a.sample(0, -9.0);
    EXPECT_DOUBLE_EQ(a.peak_pnl, 0.0);
    EXPECT_DOUBLE_EQ(a.max_drawdown, 9.0);
}
