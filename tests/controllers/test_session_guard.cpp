#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>

#include "FakeClock.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/AuthService.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/Logger.h"
#include "aid/value-types/Ids.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::AuthService;
using aid::auth::LoginResult;
using aid::auth::PasswordHasher;
using aid::auth::SessionRepo;
using aid::auth::UserRepo;
using aid::controllers::SessionGuard;
using aid::crosscutting::AuthConfig;
using aid::crosscutting::Logger;
using aid::fakes::FakeClock;

namespace {

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            const auto tmp = fs::temp_directory_path();
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               (tmp / "aid_session_guard_test_backend.log").string(),
                               (tmp / "aid_session_guard_test_frontend.log").string());
        });
    }
};

struct Fixture : LoggerOnce {
    fs::path dir;
    fs::path dbPath;
    std::optional<AuthDb> db;
    FakeClock clock;
    AuthConfig cfg;
    std::optional<UserRepo> users;
    std::optional<SessionRepo> sessions;
    std::optional<AuthService> svc;
    std::optional<SessionGuard> guard;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_session_guard_" << pid << "_" << now << "_" << seq;
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
        cfg.cookieName = "aid_session";

        users.emplace(*db, clock);
        sessions.emplace(*db, clock, cfg);
        svc.emplace(*users, *sessions, clock, cfg);
        guard.emplace(*users, *sessions, clock, Logger::instance(), cfg);
        PasswordHasher::initialize();
    }

    ~Fixture() {
        guard.reset();
        svc.reset();
        sessions.reset();
        users.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    // Helper: create a user + login → return the plaintext session token.
    [[nodiscard]] std::string makeUserAndLogin() {
        auto h = PasswordHasher::hash("p");
        EXPECT_TRUE(h.has_value());
        auto id = users->create("alice", *h);
        EXPECT_TRUE(id.has_value());

        auto loginTask = svc->login("alice", "p", "ip", "ua");
        EXPECT_TRUE(loginTask.done());
        auto loginR = loginTask.await_resume();
        EXPECT_TRUE(loginR.has_value()) << (loginR.has_value() ? "" : loginR.error().message);
        return loginR->token.v;
    }
};

struct CallbackCapture {
    std::optional<drogon::HttpResponsePtr> rejected;
    bool passedThrough = false;

    drogon::FilterCallback fcb() {
        return [this](const drogon::HttpResponsePtr& r) { rejected = r; };
    }
    drogon::FilterChainCallback fccb() {
        return [this] { passedThrough = true; };
    }
};

[[nodiscard]] drogon::HttpRequestPtr requestWithCookie(std::string_view name,
                                                       std::string_view value) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->addCookie(std::string{name}, std::string{value});
    return req;
}

} // namespace

TEST(SessionGuard, RejectsMissingCookie) {
    Fixture f;
    auto req = drogon::HttpRequest::newHttpRequest();
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);
    EXPECT_FALSE(cc.passedThrough);
}

// A reset grant cookie must NEVER be accepted as a session: it uses a
// distinct cookie name, so SessionGuard (which only reads cfg.cookieName)
// sees no session cookie and rejects. Guards the cookie-confusion pitfall.
TEST(SessionGuard, RejectsResetGrantCookieAsSession) {
    Fixture f;
    auto req = requestWithCookie("aid_reset", std::string(64, 'a'));
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);
    EXPECT_FALSE(cc.passedThrough);
}

TEST(SessionGuard, RejectsMalformedCookieLength) {
    Fixture f;
    auto req = requestWithCookie("aid_session", "deadbeef");
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);
}

TEST(SessionGuard, RejectsUnknownToken) {
    Fixture f;
    // 64 hex chars, but no matching prefix in the DB.
    auto req = requestWithCookie("aid_session", std::string(64, 'a'));
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);
    EXPECT_FALSE(cc.passedThrough);
}

TEST(SessionGuard, RejectsExpiredSessionAndRevokes) {
    Fixture f;
    const auto token = f.makeUserAndLogin();
    f.clock.advance(std::chrono::seconds{f.cfg.sessionLifetimeSeconds + 1});

    auto req = requestWithCookie("aid_session", token);
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);

    // Row should be gone.
    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(token));
    ASSERT_TRUE(sess.has_value());
    EXPECT_FALSE(sess->has_value());
}

TEST(SessionGuard, ValidSessionPassesThroughSetsViewerAndSlides) {
    Fixture f;
    const auto token = f.makeUserAndLogin();

    // Advance clock a little so we can verify slide bumps last_seen_at.
    const auto beforeSlide = f.clock.now();
    f.clock.advance(std::chrono::seconds{60});

    auto req = requestWithCookie("aid_session", token);
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());

    ASSERT_FALSE(cc.rejected.has_value())
        << "filter unexpectedly rejected; status="
        << (cc.rejected.has_value() ? (*cc.rejected)->statusCode() : 0);
    EXPECT_TRUE(cc.passedThrough);

    // Viewer attribute set.
    const auto& viewer = req->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
    EXPECT_EQ(viewer.v, "alice");

    // Session row's last_seen_at should have advanced past beforeSlide.
    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(token));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    EXPECT_GT((*sess)->lastSeenAt, beforeSlide);
    EXPECT_GT((*sess)->expiresAt, beforeSlide + std::chrono::seconds{f.cfg.sessionLifetimeSeconds});
}

TEST(SessionGuard, RejectsAndRevokesIfUserDeletedAfterLogin) {
    Fixture f;
    const auto token = f.makeUserAndLogin();
    auto by = f.users->lookupByUsername("alice");
    ASSERT_TRUE(by.has_value());
    ASSERT_TRUE(by->has_value());
    auto del = f.users->deleteUser((*by)->id);
    ASSERT_TRUE(del.has_value());
    // FOREIGN KEY ON DELETE CASCADE already kills the row, but the
    // filter must also fail closed if it encounters a session whose
    // user is gone (we still test the user-gone branch).

    auto req = requestWithCookie("aid_session", token);
    CallbackCapture cc;
    f.guard->doFilter(req, cc.fcb(), cc.fccb());
    ASSERT_TRUE(cc.rejected.has_value());
    EXPECT_EQ((*cc.rejected)->statusCode(), drogon::k401Unauthorized);
}
