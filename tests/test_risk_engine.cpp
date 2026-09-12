#include <gtest/gtest.h>

#include "risk/risk_engine.hpp"

using namespace lob;

namespace {
RiskLimits default_limits() {
    RiskLimits limits;
    limits.max_position = 500;
    limits.max_order_size = 100;
    limits.max_notional = 50'000.0;
    limits.max_daily_loss = 1'000.0;
    limits.max_open_orders = 5;
    return limits;
}
}  // namespace

TEST(RiskEngine, ApprovesOrderWithinAllLimits) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{10, Side::Buy, 100.0, 0, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::Approved);
}

TEST(RiskEngine, RejectsOrderExceedingMaxOrderSize) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{101, Side::Buy, 100.0, 0, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxOrderSize);
}

TEST(RiskEngine, RejectsZeroOrNegativeOrderSize) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{0, Side::Buy, 100.0, 0, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxOrderSize);
}

TEST(RiskEngine, RejectsOrderThatWouldBreachMaxPosition) {
    RiskEngine engine(default_limits());
    // Already long 480, buying 50 more -> 530 > max_position 500
    RiskCheckInput in{50, Side::Buy, 100.0, 480, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxPosition);
}

TEST(RiskEngine, AllowsSellThatReducesPositionEvenWhenLongIsMaxed) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{50, Side::Sell, 100.0, 500, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::Approved);
}

TEST(RiskEngine, RejectsShortBreachOfMaxPosition) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{50, Side::Sell, 100.0, -480, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxPosition);
}

TEST(RiskEngine, RejectsOrderExceedingMaxNotional) {
    RiskEngine engine(default_limits());
    // 100 shares * 600 = 60,000 > max_notional 50,000
    RiskCheckInput in{100, Side::Buy, 600.0, 0, 0.0, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxNotional);
}

TEST(RiskEngine, RejectsWhenDailyLossLimitBreached) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{10, Side::Buy, 100.0, 0, -1'000.01, 0, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxDailyLoss);
}

TEST(RiskEngine, RejectsWhenTooManyOpenOrders) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{10, Side::Buy, 100.0, 0, 0.0, 5, false};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedMaxOpenOrders);
}

TEST(RiskEngine, RejectsOnStaleMarketDataBeforeAnyOtherCheck) {
    RiskEngine engine(default_limits());
    RiskCheckInput in{10, Side::Buy, 100.0, 0, 0.0, 0, true};
    EXPECT_EQ(engine.check(in), RiskDecision::RejectedStaleMarketData);
}
