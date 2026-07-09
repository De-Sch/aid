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
#include <thread>

#include "FakeClock.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/AuthService.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::AuthService;
using aid::auth::LoginResult;
using aid::auth::PasswordHasher;
using aid::auth::SessionRepo;
using aid::auth::SessionToken;
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
    std::optional<UserRepo> users;
    std::optional<SessionRepo> sessions;
    std::optional<AuthService> svc;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_auth_service_" << pid << "_" << now << "_" << seq;
        dir = fs::temp_directory_path() / name.str();
        fs::create_directories(dir);
        dbPath = dir / "auth.db";

        auto r = AuthDb::open(dbPath);
        if (!r) {
            std::abort();
        }
        db.emplace(std::move(*r));
        clock.set(aid::Timestamp{std::chrono::seconds{1'700'000'000}});
        cfg.sessionLifetimeSeconds = 2'592'000;

        users.emplace(*db, clock);
        sessions.emplace(*db, clock, cfg);
        svc.emplace(*users, *sessions, clock, cfg);
        PasswordHasher::initialize();
    }

    ~Fixture() {
        svc.reset();
        sessions.reset();
        users.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Create a user with a known plaintext password. Returns the new id.
    std::int64_t makeUser(std::string_view name, std::string_view password) {
        auto h = PasswordHasher::hash(password);
        EXPECT_TRUE(h.has_value()) << (h.has_value() ? "" : h.error().message);
        auto id = users->create(name, *h);
        EXPECT_TRUE(id.has_value()) << (id.has_value() ? "" : id.error().message);
        return *id;
    }
};

// Drains an eager Task<T> (initial_suspend=suspend_never, no co_await
// inside) to its value. AuthService bodies are synchronous SQLite +
// libsodium calls; the Task<> shape exists only for uniformity with
// the rest of the daemon.
template <class T> T drain(aid::plumbing::Task<T>&& t) {
    EXPECT_TRUE(t.done());
    return t.await_resume();
}

inline void drainVoid(aid::plumbing::Task<aid::plumbing::Result<void>>&& t) {
    EXPECT_TRUE(t.done());
    (void)t.await_resume();
}

} // namespace

TEST(AuthService, LoginSuccessReturns64HexTokenAndUpdatesLastLogin) {
    Fixture f;
    f.makeUser("alice", "correct horse battery staple");

    auto r = drain(f.svc->login("alice", "correct horse battery staple", "10.0.0.1", "ua"));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->token.v.size(), 64U);
    for (char c : r->token.v) {
        EXPECT_TRUE(std::isxdigit(static_cast<unsigned char>(c)));
    }
    EXPECT_EQ(r->ipAtLogin, std::optional<std::string>{"10.0.0.1"});
    EXPECT_EQ(r->userAgent, std::optional<std::string>{"ua"});
    EXPECT_EQ(r->expiresAt, f.clock.now() + std::chrono::seconds{f.cfg.sessionLifetimeSeconds});

    auto by = f.users->lookupByUsername("alice");
    ASSERT_TRUE(by.has_value());
    ASSERT_TRUE(by->has_value());
    ASSERT_TRUE((*by)->lastLoginAt.has_value());
}

TEST(AuthService, LoginUnknownUserReturnsUnauthenticatedAndDoesNotMutate) {
    Fixture f;
    // Note: no user created.
    auto r = drain(f.svc->login("ghost", "anything", "ip", "ua"));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Unauthenticated);

    // No row should appear; sanity check.
    auto by = f.users->lookupByUsername("ghost");
    ASSERT_TRUE(by.has_value());
    EXPECT_FALSE(by->has_value());
}

TEST(AuthService, LoginEmptyPasswordReturnsUnauthenticated) {
    Fixture f;
    f.makeUser("alice", "right");

    auto r = drain(f.svc->login("alice", "", "ip", "ua"));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::Unauthenticated);
}

// Per L7 (security review), there is no failed-attempt counter and no
// auto-lockout. A wrong password gets the same error as success on a
// later good attempt — no enumeration leak, no DoS-against-operator.
TEST(AuthService, LoginWrongPasswordDoesNotMutateUserState) {
    Fixture f;
    f.makeUser("alice", "right");

    for (int i = 0; i < 10; ++i) {
        auto r = drain(f.svc->login("alice", "wrong", "ip", "ua"));
        ASSERT_FALSE(r.has_value());
        EXPECT_EQ(r.error().code, ErrorCode::Unauthenticated);
    }

    // The correct password still works — no lockout intervening.
    auto good = drain(f.svc->login("alice", "right", "ip", "ua"));
    ASSERT_TRUE(good.has_value()) << good.error().message;
}

TEST(AuthService, LogoutRevokesSessionAndIsIdempotent) {
    Fixture f;
    f.makeUser("alice", "p");
    auto loginR = drain(f.svc->login("alice", "p", "ip", "ua"));
    ASSERT_TRUE(loginR.has_value()) << loginR.error().message;
    const auto token = loginR->token;

    drainVoid(f.svc->logout(token));

    // whoami should now fail.
    auto w = drain(f.svc->whoami(token));
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().code, ErrorCode::Unauthenticated);

    // Second logout — still succeeds, no error.
    drainVoid(f.svc->logout(token));
}

TEST(AuthService, WhoamiReturnsUserHandleForValidSession) {
    Fixture f;
    f.makeUser("alice", "p");
    auto loginR = drain(f.svc->login("alice", "p", "ip", "ua"));
    ASSERT_TRUE(loginR.has_value());

    auto w = drain(f.svc->whoami(loginR->token));
    ASSERT_TRUE(w.has_value()) << w.error().message;
    EXPECT_EQ(w->v, "alice");
}

TEST(AuthService, WhoamiRejectsMalformedToken) {
    Fixture f;
    auto w = drain(f.svc->whoami(SessionToken{"short"}));
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().code, ErrorCode::Unauthenticated);
}

// H1 — login concurrency cap. With maxConcurrentLogins=1, while
// thread A is mid-Argon2 (which takes hundreds of ms at MODERATE), a
// concurrent call from the main thread must bounce with
// TooManyRequests. Argon2 is intentionally slow, which gives us a
// reliable window without needing a barrier into AuthService internals.
TEST(AuthService, LoginThrottlesWhenConcurrencyCapReached) {
    Fixture f;
    f.cfg.maxConcurrentLogins = 1;
    // Local AuthService so we can override the cap after the
    // fixture's default-constructed cfg.
    AuthService svc{*f.users, *f.sessions, f.clock, f.cfg};
    f.makeUser("alice", "p");

    std::atomic<bool> firstEntered{false};
    std::atomic<bool> firstOk{false};

    std::thread t([&] {
        firstEntered.store(true, std::memory_order_release);
        auto r = drain(svc.login("alice", "p", "ip", "ua"));
        firstOk.store(r.has_value(), std::memory_order_release);
    });

    // Wait for thread t to have entered login() and let it acquire the slot,
    // then fire the second login while t is still inside its Argon2 verify.
    // PasswordHasher now runs at 32 MiB / ops=3 (~tens of ms), so the sleep
    // must land *inside* that window: 5 ms is comfortably shorter than a
    // memory-hard 32 MiB hash (which cannot complete in <5 ms), and t acquires
    // the slot within a sub-ms SQLite lookup of setting firstEntered. REASON:
    // the only fully-deterministic alternative would expose AuthService's
    // private semaphore (or a test hook), which we want to avoid. If the Argon2
    // cost is ever lowered further, shrink this sleep to match.
    while (!firstEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto r2 = drain(svc.login("alice", "p", "ip", "ua"));
    ASSERT_FALSE(r2.has_value());
    EXPECT_EQ(r2.error().code, ErrorCode::TooManyRequests);

    t.join();
    EXPECT_TRUE(firstOk.load(std::memory_order_acquire))
        << "the in-flight login should not have been thrown by the throttle";
}

// Sequential logins under cap=1 both succeed — the RAII release on
// the semaphore slot must run on every exit path.
TEST(AuthService, LoginThrottleSlotIsReleasedSoSequentialCallsSucceed) {
    Fixture f;
    f.cfg.maxConcurrentLogins = 1;
    AuthService svc{*f.users, *f.sessions, f.clock, f.cfg};
    f.makeUser("alice", "p");

    auto r1 = drain(svc.login("alice", "p", "ip", "ua"));
    ASSERT_TRUE(r1.has_value()) << r1.error().message;

    auto r2 = drain(svc.login("alice", "p", "ip", "ua"));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
}

// Even on the bad-creds path the slot must be released — otherwise
// an attacker probing wrong passwords would exhaust the cap.
TEST(AuthService, LoginThrottleSlotReleasedOnBadCreds) {
    Fixture f;
    f.cfg.maxConcurrentLogins = 1;
    AuthService svc{*f.users, *f.sessions, f.clock, f.cfg};
    f.makeUser("alice", "right");

    auto r1 = drain(svc.login("alice", "wrong", "ip", "ua"));
    ASSERT_FALSE(r1.has_value());
    EXPECT_EQ(r1.error().code, ErrorCode::Unauthenticated);

    // Slot must be back; correct credentials now succeed.
    auto r2 = drain(svc.login("alice", "right", "ip", "ua"));
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
}

TEST(AuthService, WhoamiRejectsExpiredSession) {
    Fixture f;
    f.makeUser("alice", "p");
    auto loginR = drain(f.svc->login("alice", "p", "ip", "ua"));
    ASSERT_TRUE(loginR.has_value());

    f.clock.advance(std::chrono::seconds{f.cfg.sessionLifetimeSeconds + 1});
    auto w = drain(f.svc->whoami(loginR->token));
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().code, ErrorCode::Unauthenticated);
}

// --- Recovery key (master password) ---------------------------------------

namespace {
constexpr const char* kRecoveryKey = "master-recovery-key";

void setRecoveryKey(AuthConfig& cfg) {
    auto h = PasswordHasher::hash(kRecoveryKey);
    ASSERT_TRUE(h.has_value());
    cfg.recoveryKeyHash = *h;
}
} // namespace

TEST(AuthService, TryRecoveryKeyDisabledReturnsFalseWithoutWork) {
    Fixture f;
    // No recoveryKeyHash configured.
    auto r = drain(f.svc->tryRecoveryKey(kRecoveryKey));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(*r);
}

TEST(AuthService, TryRecoveryKeyMatchesConfiguredKey) {
    Fixture f;
    setRecoveryKey(f.cfg);
    auto r = drain(f.svc->tryRecoveryKey(kRecoveryKey));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_TRUE(*r);
}

TEST(AuthService, TryRecoveryKeyRejectsWrongKey) {
    Fixture f;
    setRecoveryKey(f.cfg);
    auto r = drain(f.svc->tryRecoveryKey("not-the-key"));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(*r);
}

TEST(AuthService, TryRecoveryKeyEmptyCandidateReturnsFalse) {
    Fixture f;
    setRecoveryKey(f.cfg);
    auto r = drain(f.svc->tryRecoveryKey(""));
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(*r);
}

TEST(AuthService, ApplyResetExistingChangesPasswordAndRevokesSessions) {
    Fixture f;
    f.makeUser("alice", "oldpass");
    auto loginR = drain(f.svc->login("alice", "oldpass", "ip", "ua"));
    ASSERT_TRUE(loginR.has_value()) << loginR.error().message;
    const auto token = loginR->token;

    drainVoid(f.svc->applyReset("alice", "brand-new-pass"));

    // New password works, old one does not.
    EXPECT_TRUE(drain(f.svc->login("alice", "brand-new-pass", "ip", "ua")).has_value());
    EXPECT_FALSE(drain(f.svc->login("alice", "oldpass", "ip", "ua")).has_value());

    // The pre-reset session was revoked.
    auto w = drain(f.svc->whoami(token));
    ASSERT_FALSE(w.has_value());
    EXPECT_EQ(w.error().code, ErrorCode::Unauthenticated);
}

TEST(AuthService, ApplyResetCreatesUnknownUser) {
    Fixture f;
    drainVoid(f.svc->applyReset("newbie", "newbie-password"));

    auto by = f.users->lookupByUsername("newbie");
    ASSERT_TRUE(by.has_value());
    ASSERT_TRUE(by->has_value());
    EXPECT_TRUE(drain(f.svc->login("newbie", "newbie-password", "ip", "ua")).has_value());
}

TEST(AuthService, ApplyResetEmptyPasswordReturnsInvalidInput) {
    Fixture f;
    f.makeUser("alice", "oldpass");
    auto r = drain(f.svc->applyReset("alice", ""));
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
}
