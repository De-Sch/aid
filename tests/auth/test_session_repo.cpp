#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>

#include "FakeClock.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::SessionRepo;
using aid::auth::UserRepo;
using aid::crosscutting::AuthConfig;
using aid::fakes::FakeClock;
using aid::plumbing::ErrorCode;

namespace {

struct Fixture {
    fs::path dir;
    fs::path dbPath;
    std::optional<AuthDb> db;
    FakeClock clock;
    AuthConfig cfg;
    std::int64_t userId = 0;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_sessionrepo_" << pid << "_" << now << "_" << seq;
        dir = fs::temp_directory_path() / name.str();
        fs::create_directories(dir);
        dbPath = dir / "auth.db";

        auto r = AuthDb::open(dbPath);
        if (!r) {
            std::abort();
        }
        db.emplace(std::move(*r));
        clock.set(aid::Timestamp{std::chrono::seconds{1'700'000'000}});

        cfg.sessionLifetimeSeconds = 60; // short so slide deltas are obvious

        // Seed one user — sessions FK to users(id) and we don't want
        // every test to also exercise UserRepo.
        UserRepo users{*db, clock};
        auto u = users.create("alice", "hash-irrelevant");
        if (!u) {
            std::abort();
        }
        userId = *u;
    }

    ~Fixture() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    SessionRepo makeRepo() { return SessionRepo{*db, clock, cfg}; }
};

constexpr std::string_view kHashA =
    "0000000000000000000000000000000000000000000000000000000000000aaa";
constexpr std::string_view kHashB =
    "0000000000000000000000000000000000000000000000000000000000000bbb";

} // namespace

TEST(SessionRepo, CreateAndLookupRoundtrip) {
    Fixture f;
    auto repo = f.makeRepo();
    auto s = repo.create(f.userId, kHashA, "00000000", "10.0.0.1", "ua/1.0");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    EXPECT_GT(s->id, 0);
    EXPECT_EQ(s->userId, f.userId);
    EXPECT_EQ(s->tokenHash, kHashA);
    EXPECT_EQ(s->prefix, "00000000");
    EXPECT_EQ(s->createdAt, f.clock.now());
    EXPECT_EQ(s->lastSeenAt, f.clock.now());
    ASSERT_TRUE(s->ipAtLogin.has_value());
    EXPECT_EQ(*s->ipAtLogin, "10.0.0.1");
    ASSERT_TRUE(s->userAgent.has_value());
    EXPECT_EQ(*s->userAgent, "ua/1.0");

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value()) << looked.error().message;
    ASSERT_TRUE(looked->has_value());
    EXPECT_EQ((*looked)->id, s->id);
    EXPECT_EQ((*looked)->tokenHash, kHashA);
}

TEST(SessionRepo, LookupByTokenHashReturnsNulloptForMissing) {
    Fixture f;
    auto repo = f.makeRepo();
    auto r = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->has_value());
}

TEST(SessionRepo, CreateSetsExpiresAtToNowPlusLifetime) {
    Fixture f;
    auto repo = f.makeRepo();
    const auto t0 = f.clock.now();
    auto s = repo.create(f.userId, kHashA, "00000000", "", "");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    EXPECT_EQ(s->expiresAt, t0 + std::chrono::seconds{f.cfg.sessionLifetimeSeconds});
}

TEST(SessionRepo, CreateAcceptsEmptyOptionalFields) {
    Fixture f;
    auto repo = f.makeRepo();
    auto s = repo.create(f.userId, kHashA, "00000000", "", "");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    EXPECT_FALSE(s->ipAtLogin.has_value());
    EXPECT_FALSE(s->userAgent.has_value());

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value());
    ASSERT_TRUE(looked->has_value());
    EXPECT_FALSE((*looked)->ipAtLogin.has_value());
    EXPECT_FALSE((*looked)->userAgent.has_value());
}

TEST(SessionRepo, CreateRejectsEmptyHashOrPrefix) {
    Fixture f;
    auto repo = f.makeRepo();
    EXPECT_EQ(repo.create(f.userId, "", "00000000", "", "").error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(repo.create(f.userId, kHashA, "", "", "").error().code, ErrorCode::InvalidInput);
}

TEST(SessionRepo, CreateRejectsDuplicateTokenHash) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());
    auto dup = repo.create(f.userId, kHashA, "00000000", "", "");
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, ErrorCode::Conflict);
}

TEST(SessionRepo, SlideExtendsExpiresAtAndLastSeen) {
    Fixture f;
    auto repo = f.makeRepo();
    auto s = repo.create(f.userId, kHashA, "00000000", "", "");
    ASSERT_TRUE(s.has_value()) << s.error().message;
    const auto initialExpiry = s->expiresAt;

    f.clock.advance(std::chrono::seconds{30});
    ASSERT_TRUE(repo.slide(s->id).has_value());

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value());
    ASSERT_TRUE(looked->has_value());
    EXPECT_GT((*looked)->expiresAt, initialExpiry);
    EXPECT_EQ((*looked)->expiresAt,
              f.clock.now() + std::chrono::seconds{f.cfg.sessionLifetimeSeconds});
    EXPECT_EQ((*looked)->lastSeenAt, f.clock.now());
}

TEST(SessionRepo, RevokeRemovesRow) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());
    ASSERT_TRUE(repo.revoke(kHashA).has_value());

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value());
    EXPECT_FALSE(looked->has_value());
}

TEST(SessionRepo, RevokeIsIdempotent) {
    Fixture f;
    auto repo = f.makeRepo();
    // Token never existed — revoke still succeeds.
    EXPECT_TRUE(repo.revoke(kHashA).has_value());
}

TEST(SessionRepo, RevokeAllForUserRemovesEveryRow) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());
    ASSERT_TRUE(repo.create(f.userId, kHashB, "11111111", "", "").has_value());

    ASSERT_TRUE(repo.revokeAllFor(f.userId).has_value());

    auto a = repo.lookupByTokenHash(kHashA);
    auto b = repo.lookupByTokenHash(kHashB);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(a->has_value());
    EXPECT_FALSE(b->has_value());
}

TEST(SessionRepo, PruneRemovesOnlyExpiredSessions) {
    Fixture f;
    auto repo = f.makeRepo();
    auto fresh = repo.create(f.userId, kHashA, "00000000", "", "");
    ASSERT_TRUE(fresh.has_value()) << fresh.error().message;

    f.clock.advance(std::chrono::seconds{30});
    auto younger = repo.create(f.userId, kHashB, "11111111", "", "");
    ASSERT_TRUE(younger.has_value()) << younger.error().message;

    // After advancing past fresh's expiry but before younger's:
    f.clock.advance(
        std::chrono::seconds{45}); // 30 + 45 = 75; fresh.expiresAt = 60, younger.expiresAt = 90

    auto pruned = repo.prune();
    ASSERT_TRUE(pruned.has_value()) << pruned.error().message;
    EXPECT_EQ(*pruned, 1) << "exactly the older session should be pruned";

    auto a = repo.lookupByTokenHash(kHashA);
    auto b = repo.lookupByTokenHash(kHashB);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_FALSE(a->has_value());
    EXPECT_TRUE(b->has_value());
}

TEST(SessionRepo, LookupByTokenHashReturnsRowEvenIfExpired) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());
    f.clock.advance(std::chrono::seconds{f.cfg.sessionLifetimeSeconds + 10});

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value());
    ASSERT_TRUE(looked->has_value()) << "expired sessions are still returned; SessionGuard filters";
    EXPECT_LT((*looked)->expiresAt, f.clock.now());
}

TEST(SessionRepo, SlideOnMissingSessionReturnsNotFound) {
    Fixture f;
    auto repo = f.makeRepo();
    auto r = repo.slide(9999);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(SessionRepo, PruneReturnsZeroWhenNothingExpired) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());

    auto pruned = repo.prune();
    ASSERT_TRUE(pruned.has_value()) << pruned.error().message;
    EXPECT_EQ(*pruned, 0);
}

// Regression for the security-review M1 finding: under the old
// `WHERE prefix = ?` lookup, a second session with the same prefix as
// an existing one would have been shadowed (SQLite returned the first
// match in implementation-defined order, and the caller-side
// `tokenHash != stored` compare returned 401 for the legitimate one).
// With token_hash as the lookup key, both look up cleanly.
TEST(SessionRepo, BothSessionsLookupCleanlyWhenPrefixesCollide) {
    Fixture f;
    auto repo = f.makeRepo();
    constexpr std::string_view kCollidingPrefix = "abcdef01";
    constexpr std::string_view kHashColliderA =
        "abcdef01000000000000000000000000000000000000000000000000000000aa";
    constexpr std::string_view kHashColliderB =
        "abcdef01000000000000000000000000000000000000000000000000000000bb";

    ASSERT_TRUE(repo.create(f.userId, kHashColliderA, kCollidingPrefix, "", "").has_value());
    ASSERT_TRUE(repo.create(f.userId, kHashColliderB, kCollidingPrefix, "", "").has_value());

    auto a = repo.lookupByTokenHash(kHashColliderA);
    ASSERT_TRUE(a.has_value()) << a.error().message;
    ASSERT_TRUE(a->has_value());
    EXPECT_EQ((*a)->tokenHash, kHashColliderA);

    auto b = repo.lookupByTokenHash(kHashColliderB);
    ASSERT_TRUE(b.has_value()) << b.error().message;
    ASSERT_TRUE(b->has_value());
    EXPECT_EQ((*b)->tokenHash, kHashColliderB);

    // The two rows must be distinct — under the old prefix-keyed
    // lookup the second one would have been shadowed by the first
    // and tested unequal on tokenHash (the legitimate session would
    // be unable to authenticate).
    EXPECT_NE((*a)->id, (*b)->id);
}

TEST(SessionRepo, SessionsCascadeOnUserDelete) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create(f.userId, kHashA, "00000000", "", "").has_value());

    UserRepo users{*f.db, f.clock};
    ASSERT_TRUE(users.deleteUser(f.userId).has_value());

    auto looked = repo.lookupByTokenHash(kHashA);
    ASSERT_TRUE(looked.has_value());
    EXPECT_FALSE(looked->has_value()) << "ON DELETE CASCADE should have removed the session row";
}
