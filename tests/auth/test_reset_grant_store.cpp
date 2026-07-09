#include <gtest/gtest.h>

#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "FakeClock.h"
#include "aid/auth/ResetGrantStore.h"
#include "aid/value-types/Ids.h"

using aid::auth::ResetGrantStore;
using aid::fakes::FakeClock;

namespace {
aid::Timestamp epoch(long long secs) {
    return aid::Timestamp{std::chrono::seconds{secs}};
}
} // namespace

TEST(ResetGrantStore, IssueThenConsumeReturnsBoundUsername) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock};

    const std::string token = store.issue("alice");
    EXPECT_EQ(token.size(), 64U);

    auto who = store.consume(token);
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(who->v, "alice");
}

TEST(ResetGrantStore, ConsumeUnknownTokenReturnsNullopt) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock};

    EXPECT_FALSE(store.consume("deadbeef").has_value());
    EXPECT_FALSE(store.consume("").has_value());
}

TEST(ResetGrantStore, GrantIsSingleUse) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock};

    const std::string token = store.issue("alice");
    ASSERT_TRUE(store.consume(token).has_value());
    // Second consume of the same token finds nothing.
    EXPECT_FALSE(store.consume(token).has_value());
}

TEST(ResetGrantStore, ExpiredGrantReturnsNullopt) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock, /*ttlSeconds=*/10};

    const std::string token = store.issue("alice");
    clock.advance(std::chrono::seconds{11});
    EXPECT_FALSE(store.consume(token).has_value());
}

TEST(ResetGrantStore, GrantLivesUntilExactlyTtl) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock, /*ttlSeconds=*/10};

    const std::string token = store.issue("alice");
    clock.advance(std::chrono::seconds{9}); // still inside the window
    auto who = store.consume(token);
    ASSERT_TRUE(who.has_value());
    EXPECT_EQ(who->v, "alice");
}

TEST(ResetGrantStore, GrantsForDifferentUsernamesAreIndependent) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock};

    const std::string a = store.issue("alice");
    const std::string b = store.issue("bob");

    auto whoB = store.consume(b);
    ASSERT_TRUE(whoB.has_value());
    EXPECT_EQ(whoB->v, "bob");

    // Consuming bob's grant did not disturb alice's.
    auto whoA = store.consume(a);
    ASSERT_TRUE(whoA.has_value());
    EXPECT_EQ(whoA->v, "alice");
}

// Concurrency smoke: many threads issue and consume in parallel. Under
// the sanitized build (TSan/ASan) this guards the mutex discipline.
TEST(ResetGrantStore, ConcurrentIssueConsumeIsSafe) {
    FakeClock clock{epoch(1'700'000'000)};
    ResetGrantStore store{clock};

    constexpr int kThreads = 16;
    std::vector<std::thread> threads;
    std::atomic<int> matched{0};
    threads.reserve(kThreads);
    for (int i = 0; i < kThreads; ++i) {
        threads.emplace_back([&store, &matched, i] {
            const std::string user = "user" + std::to_string(i);
            const std::string token = store.issue(user);
            auto who = store.consume(token);
            if (who.has_value() && who->v == user) {
                matched.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    for (auto& t : threads) {
        t.join();
    }
    EXPECT_EQ(matched.load(), kThreads);
}
