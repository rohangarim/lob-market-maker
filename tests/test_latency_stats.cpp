#include <gtest/gtest.h>

#include "metrics/latency_stats.hpp"

using namespace lob;

TEST(LatencyStats, EmptyReturnsZeros) {
    LatencyStats s;
    EXPECT_EQ(s.count(), 0u);
    EXPECT_DOUBLE_EQ(s.percentile(50), 0.0);
    EXPECT_DOUBLE_EQ(s.max_ns(), 0.0);
    EXPECT_DOUBLE_EQ(s.mean_ns(), 0.0);
}

TEST(LatencyStats, SmallValuesAreExact) {
    LatencyStats s;
    for (int v = 0; v < 32; ++v) s.add_ns(v);
    EXPECT_DOUBLE_EQ(s.percentile(0), 0.0);
    EXPECT_DOUBLE_EQ(s.percentile(100), 31.0);
    EXPECT_DOUBLE_EQ(s.max_ns(), 31.0);
    EXPECT_DOUBLE_EQ(s.mean_ns(), 15.5);
}

TEST(LatencyStats, LargeValuesWithinSixPercentBelow) {
    for (int64_t v : {33LL, 83LL, 1000LL, 12345LL, 9'999'999LL, 5'000'000'000LL}) {
        LatencyStats s;
        s.add_ns(v);
        double p = s.percentile(50);
        EXPECT_LE(p, static_cast<double>(v));
        EXPECT_GE(p, static_cast<double>(v) * (1.0 - 1.0 / 16.0)) << "v=" << v;
        EXPECT_DOUBLE_EQ(s.max_ns(), static_cast<double>(v));
    }
}

TEST(LatencyStats, PercentilesSeparateBulkFromOutlier) {
    LatencyStats s;
    for (int i = 0; i < 99; ++i) s.add_ns(100);
    s.add_ns(100000);
    EXPECT_LE(s.percentile(50), 100.0);
    EXPECT_GE(s.percentile(50), 93.0);
    EXPECT_LE(s.percentile(99), 100.0);   // 99th of 100 samples is still the bulk
    EXPECT_GE(s.percentile(100), 93750.0);  // the max sample's bucket
    EXPECT_DOUBLE_EQ(s.max_ns(), 100000.0);
}

TEST(LatencyStats, NegativeValuesClampToZero) {
    LatencyStats s;
    s.add_ns(-5);
    EXPECT_EQ(s.count(), 1u);
    EXPECT_DOUBLE_EQ(s.max_ns(), 0.0);
}

TEST(LatencyStats, ClearResets) {
    LatencyStats s;
    s.add_ns(500);
    s.clear();
    EXPECT_EQ(s.count(), 0u);
    EXPECT_DOUBLE_EQ(s.max_ns(), 0.0);
}

TEST(LatencyStats, HandlesVeryLargeValuesWithoutOverflowingBuckets) {
    LatencyStats s;
    s.add_ns(INT64_MAX);
    EXPECT_GT(s.percentile(50), 0.0);
}
