#include <gtest/gtest.h>

#include <chrono>

#include "aid/crosscutting/Clock.h"

using aid::crosscutting::Clock;
using aid::crosscutting::RealClock;

TEST(RealClock, TwoConsecutiveCallsAreMonotonicNonDecreasing) {
    RealClock c;
    const auto t1 = c.now();
    const auto t2 = c.now();
    // system_clock is not strictly monotonic (NTP can step it back), so
    // assert >=, not >.
    EXPECT_GE(t2.time_since_epoch().count(), t1.time_since_epoch().count());
}

TEST(RealClock, NowMatchesSystemClockNowWithinOneSecond) {
    RealClock c;
    const auto fromClock = c.now();
    const auto fromStd = std::chrono::system_clock::now();
    const auto delta = std::chrono::abs(fromStd - fromClock);
    EXPECT_LT(delta, std::chrono::seconds{1});
}

TEST(Clock, PolymorphicCallThroughBaseReferenceReturnsRealClockValue) {
    RealClock real;
    const Clock& base = real;
    const auto via_base = base.now();
    const auto via_real = real.now();
    EXPECT_LE(via_base.time_since_epoch().count(), via_real.time_since_epoch().count());
}
