#include <gtest/gtest.h>

#include "aid/version.hpp"

TEST(Smoke, VersionIsNonEmpty) {
    EXPECT_FALSE(aid::version().empty());
}

TEST(Smoke, VersionStartsWithDigit) {
    const auto v = aid::version();
    ASSERT_FALSE(v.empty());
    EXPECT_GE(v.front(), '0');
    EXPECT_LE(v.front(), '9');
}
