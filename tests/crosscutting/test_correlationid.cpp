#include <gtest/gtest.h>

#include <regex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "aid/crosscutting/CorrelationId.h"

using aid::crosscutting::CorrelationId;

TEST(CorrelationId, NextUuidIsThirtySixCharsWithCanonicalHyphenLayout) {
    const auto u = CorrelationId::nextUuid();
    ASSERT_EQ(u.size(), 36u);
    EXPECT_EQ(u[8], '-');
    EXPECT_EQ(u[13], '-');
    EXPECT_EQ(u[18], '-');
    EXPECT_EQ(u[23], '-');

    static const std::regex re{"^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$"};
    EXPECT_TRUE(std::regex_match(u, re)) << "got: " << u;
}

TEST(CorrelationId, NextUuidHasVersion4AndRfc4122Variant) {
    const auto u = CorrelationId::nextUuid();
    // Nibble 14 (1-based 15th char, 0-based index 14) is the version.
    // Layout: xxxxxxxx-xxxx-Mxxx-... → M is at index 14.
    EXPECT_EQ(u[14], '4') << "version nibble was: " << u[14] << " in " << u;

    // Nibble at index 19 is the variant; high two bits must be 10b →
    // one of {8, 9, a, b}.
    const char variant = u[19];
    EXPECT_TRUE(variant == '8' || variant == '9' || variant == 'a' || variant == 'b')
        << "variant nibble was: " << variant << " in " << u;
}

TEST(CorrelationId, OneThousandSequentialDrawsAreUnique) {
    std::set<std::string> seen;
    for (int i = 0; i < 1000; ++i) {
        seen.insert(CorrelationId::nextUuid());
    }
    EXPECT_EQ(seen.size(), 1000u);
}

TEST(CorrelationId, TwoThreadsProduceDistinctIds) {
    constexpr int kPerThread = 500;
    std::vector<std::string> a;
    std::vector<std::string> b;
    a.reserve(kPerThread);
    b.reserve(kPerThread);

    std::thread t1([&] {
        for (int i = 0; i < kPerThread; ++i)
            a.push_back(CorrelationId::nextUuid());
    });
    std::thread t2([&] {
        for (int i = 0; i < kPerThread; ++i)
            b.push_back(CorrelationId::nextUuid());
    });
    t1.join();
    t2.join();

    std::set<std::string> seen;
    for (auto& s : a)
        seen.insert(s);
    for (auto& s : b)
        seen.insert(s);
    EXPECT_EQ(seen.size(), static_cast<std::size_t>(2 * kPerThread));
}
