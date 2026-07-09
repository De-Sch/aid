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
#include <nlohmann/json.hpp>
#include <optional>
#include <sstream>
#include <string>
#include <thread>

#include "FakeClock.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/AuthService.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/ResetGrantStore.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/controllers/LoginController.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/value-types/Ids.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::AuthService;
using aid::auth::PasswordHasher;
using aid::auth::ResetGrantStore;
using aid::auth::SessionRepo;
using aid::auth::UserRepo;
using aid::controllers::LoginController;
using aid::controllers::SessionGuard;
using aid::crosscutting::AuthConfig;
using aid::crosscutting::CorrelationId;
using aid::crosscutting::Logger;
using aid::fakes::FakeClock;

namespace {

struct LoggerOnce {
    LoggerOnce() {
        static std::once_flag flag;
        std::call_once(flag, [] {
            const auto tmp = fs::temp_directory_path();
            Logger::initialize(aid::crosscutting::LogLevel::ERROR,
                               (tmp / "aid_login_ctrl_test_backend.log").string(),
                               (tmp / "aid_login_ctrl_test_frontend.log").string());
        });
    }
};

struct Fixture : LoggerOnce {
    fs::path dir;
    fs::path dbPath;
    std::optional<AuthDb> db;
    FakeClock clock;
    AuthConfig cfg;
    CorrelationId cid;
    std::optional<UserRepo> users;
    std::optional<SessionRepo> sessions;
    std::optional<AuthService> svc;
    std::optional<ResetGrantStore> grants;
    std::optional<LoginController> ctrl;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_login_ctrl_" << pid << "_" << now << "_" << seq;
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
        cfg.cookieSecure = true;

        users.emplace(*db, clock);
        sessions.emplace(*db, clock, cfg);
        svc.emplace(*users, *sessions, clock, cfg);
        grants.emplace(clock); // default 300s TTL; tests advance the FakeClock to expire
        ctrl.emplace(*svc, *grants, Logger::instance(), cid, cfg);
        PasswordHasher::initialize();
    }

    ~Fixture() {
        ctrl.reset();
        grants.reset();
        svc.reset();
        sessions.reset();
        users.reset();
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    void makeUser(std::string_view name, std::string_view password) {
        auto h = PasswordHasher::hash(password);
        ASSERT_TRUE(h.has_value());
        auto id = users->create(name, *h);
        ASSERT_TRUE(id.has_value());
    }
};

[[nodiscard]] drogon::HttpRequestPtr jsonRequest(std::string_view body) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(std::string{body});
    req->addHeader("Content-Type", "application/json");
    return req;
}

[[nodiscard]] drogon::HttpResponsePtr invokeLogin(LoginController& ctrl,
                                                  const drogon::HttpRequestPtr& req) {
    drogon::HttpResponsePtr captured;
    ctrl.postLogin(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    return captured;
}

[[nodiscard]] drogon::HttpResponsePtr invokeLogout(LoginController& ctrl,
                                                   const drogon::HttpRequestPtr& req) {
    drogon::HttpResponsePtr captured;
    ctrl.postLogout(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    return captured;
}

[[nodiscard]] drogon::HttpResponsePtr invokeReset(LoginController& ctrl,
                                                  const drogon::HttpRequestPtr& req) {
    drogon::HttpResponsePtr captured;
    ctrl.postReset(req, [&captured](const drogon::HttpResponsePtr& r) { captured = r; });
    return captured;
}

// A valid Argon2id hash of this key drives the recovery-key path. The
// plaintext is what a tester would type into the login password field.
constexpr const char* kRecoveryKey = "master-recovery-key";

[[nodiscard]] drogon::HttpRequestPtr resetRequest(std::string_view grantCookie,
                                                  std::string_view body) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    req->setBody(std::string{body});
    req->addHeader("Content-Type", "application/json");
    if (!grantCookie.empty()) {
        req->addCookie("aid_reset", std::string{grantCookie});
    }
    return req;
}

[[nodiscard]] const drogon::Cookie* findCookie(const drogon::HttpResponsePtr& resp,
                                               std::string_view name) {
    const auto& cookies = resp->getCookies();
    const auto it = cookies.find(std::string{name});
    if (it == cookies.end()) {
        return nullptr;
    }
    return &it->second;
}

} // namespace

TEST(LoginController, MalformedJsonReturns400) {
    Fixture f;
    auto req = jsonRequest("not json");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
}

TEST(LoginController, MissingUsernameReturns400) {
    Fixture f;
    auto req = jsonRequest(R"({"password":"p"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
}

TEST(LoginController, MissingPasswordReturns400) {
    Fixture f;
    auto req = jsonRequest(R"({"username":"alice"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
}

TEST(LoginController, EmptyFieldsReturn400) {
    Fixture f;
    auto req = jsonRequest(R"({"username":"","password":""})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
}

TEST(LoginController, InvalidCredentialsReturn401WithoutSetCookie) {
    Fixture f;
    f.makeUser("alice", "right");

    auto req = jsonRequest(R"({"username":"alice","password":"wrong"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(findCookie(resp, "aid_session"), nullptr);
}

TEST(LoginController, SuccessfulLoginReturns200AndCookieWithExpectedAttrs) {
    Fixture f;
    f.makeUser("alice", "p");

    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);

    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);
    EXPECT_EQ(cookie->value().size(), 64U);
    EXPECT_TRUE(cookie->isHttpOnly());
    EXPECT_TRUE(cookie->isSecure()); // cookieSecure=true
    EXPECT_EQ(cookie->sameSite(), drogon::Cookie::SameSite::kStrict);
    EXPECT_EQ(cookie->path(), "/");
    ASSERT_TRUE(cookie->maxAge().has_value());
    EXPECT_EQ(*cookie->maxAge(), 2'592'000);

    // Body shape.
    auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_TRUE(body["ok"].get<bool>());
}

TEST(LoginController, SuccessfulLoginInsecureModeOmitsSecure) {
    Fixture f;
    f.cfg.cookieSecure = false;
    f.makeUser("alice", "p");

    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);

    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);
    EXPECT_FALSE(cookie->isSecure());
}

TEST(LoginController, LogoutWithoutCookieReturns200Idempotent) {
    Fixture f;
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    auto resp = invokeLogout(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);
    ASSERT_TRUE(cookie->maxAge().has_value());
    EXPECT_EQ(*cookie->maxAge(), 0);
}

TEST(LoginController, LogoutWithValidCookieRevokesAndSetsExpiredCookie) {
    Fixture f;
    f.makeUser("alice", "p");
    auto loginReq = jsonRequest(R"({"username":"alice","password":"p"})");
    auto loginResp = invokeLogin(*f.ctrl, loginReq);
    ASSERT_EQ(loginResp->statusCode(), drogon::k200OK);
    const auto* loginCookie = findCookie(loginResp, "aid_session");
    ASSERT_NE(loginCookie, nullptr);
    const std::string token = loginCookie->value();
    EXPECT_EQ(token.size(), 64U);

    auto logoutReq = drogon::HttpRequest::newHttpRequest();
    logoutReq->setMethod(drogon::Post);
    logoutReq->addCookie("aid_session", token);
    auto logoutResp = invokeLogout(*f.ctrl, logoutReq);
    ASSERT_TRUE(logoutResp);
    EXPECT_EQ(logoutResp->statusCode(), drogon::k200OK);
    const auto* expired = findCookie(logoutResp, "aid_session");
    ASSERT_NE(expired, nullptr);
    ASSERT_TRUE(expired->maxAge().has_value());
    EXPECT_EQ(*expired->maxAge(), 0);

    // Session row should be gone.
    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(token));
    ASSERT_TRUE(sess.has_value());
    EXPECT_FALSE(sess->has_value());
}

// H1 — the throttle's response surfaces as HTTP 429 through the
// controller. Mirrors the AuthService LoginThrottlesWhenConcurrencyCapReached
// test, but goes through the controller's status-mapping switch.
TEST(LoginController, ThrottleReturns429WhileSlotHeld) {
    Fixture f;
    f.cfg.maxConcurrentLogins = 1;
    AuthService svcLocal{*f.users, *f.sessions, f.clock, f.cfg};
    LoginController ctrlLocal{svcLocal, *f.grants, Logger::instance(), f.cid, f.cfg};
    f.makeUser("alice", "p");

    std::atomic<bool> firstEntered{false};
    std::atomic<int> firstStatus{0};

    std::thread t([&] {
        firstEntered.store(true, std::memory_order_release);
        auto req = jsonRequest(R"({"username":"alice","password":"p"})");
        auto resp = invokeLogin(ctrlLocal, req);
        if (resp) {
            firstStatus.store(static_cast<int>(resp->statusCode()), std::memory_order_release);
        }
    });

    // See AuthService test for the rationale on the timing window — the second
    // login must fire while the first is still inside its Argon2 verify. With
    // the 32 MiB / ops=3 cost the verify is ~tens of ms, so 5 ms lands safely
    // inside it (a memory-hard 32 MiB hash cannot finish in <5 ms).
    while (!firstEntered.load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    auto req2 = jsonRequest(R"({"username":"alice","password":"p"})");
    auto resp2 = invokeLogin(ctrlLocal, req2);
    ASSERT_TRUE(resp2);
    EXPECT_EQ(resp2->statusCode(), drogon::k429TooManyRequests);

    t.join();
    EXPECT_EQ(firstStatus.load(std::memory_order_acquire), drogon::k200OK)
        << "in-flight login should not have been thrown by the throttle";
}

TEST(LoginController, LogoutCookieCarriesSecurityAttributes) {
    Fixture f;
    f.cfg.cookieSecure = true;
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(drogon::Post);
    auto resp = invokeLogout(*f.ctrl, req);
    ASSERT_TRUE(resp);
    const auto* expired = findCookie(resp, "aid_session");
    ASSERT_NE(expired, nullptr);
    EXPECT_TRUE(expired->isHttpOnly());
    EXPECT_TRUE(expired->isSecure());
    EXPECT_EQ(expired->sameSite(), drogon::Cookie::SameSite::kStrict);
    EXPECT_EQ(expired->path(), "/");
}

TEST(LoginController, LongForwardedForIsTruncatedToAuditCap) {
    Fixture f;
    // M2: XFF is only honored when the peer is in the proxy allowlist
    // AND the trustForwardedFor flag is on. Enable both for this test
    // so the truncation path is reachable.
    auto probe = jsonRequest("{}");
    f.cfg.trustForwardedFor = true;
    f.cfg.trustedProxyAddresses = {probe->getPeerAddr().toIp()};

    f.makeUser("alice", "p");
    const std::string longIp(1024, '7');
    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("X-Forwarded-For", longIp);
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    ASSERT_TRUE((*sess)->ipAtLogin.has_value());
    EXPECT_EQ((*sess)->ipAtLogin->size(), 256U);
}

// M2 — the default config does NOT trust X-Forwarded-For; the audit
// column reflects the TCP peer, even when XFF is present.
TEST(LoginController, ForwardedForIgnoredWhenTrustForwardedForIsFalse) {
    Fixture f;
    // Default: cfg.trustForwardedFor == false.
    f.makeUser("alice", "p");
    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("X-Forwarded-For", "203.0.113.42");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    // The header value must NOT make it into the audit column.
    EXPECT_NE((*sess)->ipAtLogin.value_or(""), "203.0.113.42");
}

// M2 — even with the flag on, XFF is ignored when the TCP peer is
// not in the proxy allowlist.
TEST(LoginController, ForwardedForIgnoredWhenPeerNotInAllowlist) {
    Fixture f;
    f.cfg.trustForwardedFor = true;
    f.cfg.trustedProxyAddresses = {"198.51.100.99"}; // deliberately not the synthetic peer.

    f.makeUser("alice", "p");
    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("X-Forwarded-For", "203.0.113.42");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    EXPECT_NE((*sess)->ipAtLogin.value_or(""), "203.0.113.42");
}

// M2 — happy path: flag on, peer in allowlist, XFF's leading value
// is recorded.
TEST(LoginController, ForwardedForHonoredWhenPeerInAllowlist) {
    Fixture f;
    auto probe = jsonRequest("{}");
    const std::string peerIp = probe->getPeerAddr().toIp();
    f.cfg.trustForwardedFor = true;
    f.cfg.trustedProxyAddresses = {peerIp};

    f.makeUser("alice", "p");
    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("X-Forwarded-For", "203.0.113.42, 198.51.100.7");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    ASSERT_TRUE((*sess)->ipAtLogin.has_value());
    EXPECT_EQ(*(*sess)->ipAtLogin, "203.0.113.42")
        << "leading XFF entry (before the first comma) wins";
}

TEST(LoginController, MultibyteUserAgentTruncatesAtUtf8Boundary) {
    Fixture f;
    f.makeUser("alice", "p");
    // Build a UA that is exactly 257 bytes by repeating a 2-byte
    // code point ("Ü" = 0xC3 0x9C). At byte 256 a naive resize would
    // land in the middle of a sequence — the UTF-8-aware truncation
    // must drop the half byte and stop at 255.
    std::string ua;
    while (ua.size() < 257) {
        ua.append("\xC3\x9C");
    }
    ASSERT_EQ(ua.size(), 258U);

    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("User-Agent", ua);
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    ASSERT_TRUE((*sess)->userAgent.has_value());
    // 256 bytes max, must land on an even (Ü-sized) boundary.
    EXPECT_LE((*sess)->userAgent->size(), 256U);
    EXPECT_EQ((*sess)->userAgent->size() % 2, 0U);
}

TEST(LoginController, LongUserAgentIsTruncatedToAuditCap) {
    Fixture f;
    f.makeUser("alice", "p");
    const std::string longUa(1024, 'X');
    auto req = jsonRequest(R"({"username":"alice","password":"p"})");
    req->addHeader("User-Agent", longUa);
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    ASSERT_EQ(resp->statusCode(), drogon::k200OK);
    const auto* cookie = findCookie(resp, "aid_session");
    ASSERT_NE(cookie, nullptr);

    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(cookie->value()));
    ASSERT_TRUE(sess.has_value());
    ASSERT_TRUE(sess->has_value());
    ASSERT_TRUE((*sess)->userAgent.has_value());
    EXPECT_EQ((*sess)->userAgent->size(), 256U);
}

TEST(LoginController, WhoamiReturnsUsernameWhenViewerAttributeSet) {
    Fixture f;
    auto req = drogon::HttpRequest::newHttpRequest();
    req->attributes()->insert(SessionGuard::VIEWER_KEY, aid::UserHandle{"alice"});

    drogon::HttpResponsePtr resp;
    f.ctrl->getWhoami(req, [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);

    auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_EQ(body["username"].get<std::string>(), "alice");
}

TEST(LoginController, WhoamiReturns401WhenViewerMissing) {
    Fixture f;
    auto req = drogon::HttpRequest::newHttpRequest();

    drogon::HttpResponsePtr resp;
    f.ctrl->getWhoami(req, [&](const drogon::HttpResponsePtr& r) { resp = r; });
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
}

// ---------------------------------------------------------------------------
// Recovery-key reset / bootstrap flow
// ---------------------------------------------------------------------------

// Enable the recovery key by writing its hash into the (referenced) cfg.
// svc holds `const AuthConfig&`, so the change is visible without
// rebuilding the service.
void enableRecoveryKey(Fixture& f) {
    auto h = PasswordHasher::hash(kRecoveryKey);
    ASSERT_TRUE(h.has_value());
    f.cfg.recoveryKeyHash = *h;
}

TEST(LoginController, RecoveryKeyLoginReturnsResetRequiredAndGrantCookie) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "realpass");

    auto req = jsonRequest(R"({"username":"alice","password":"master-recovery-key"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);

    auto body = nlohmann::json::parse(resp->getBody());
    EXPECT_TRUE(body.value("resetRequired", false));

    // A grant cookie, not a session cookie.
    EXPECT_EQ(findCookie(resp, "aid_session"), nullptr);
    const auto* grant = findCookie(resp, "aid_reset");
    ASSERT_NE(grant, nullptr);
    EXPECT_EQ(grant->value().size(), 64U);
    EXPECT_TRUE(grant->isHttpOnly());
    EXPECT_TRUE(grant->isSecure()); // cookieSecure=true
    EXPECT_EQ(grant->sameSite(), drogon::Cookie::SameSite::kStrict);
    EXPECT_EQ(grant->path(), "/");
    ASSERT_TRUE(grant->maxAge().has_value());
    EXPECT_EQ(*grant->maxAge(), 300);
}

TEST(LoginController, NormalLoginStillWorksWhenRecoveryConfigured) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "realpass");

    auto req = jsonRequest(R"({"username":"alice","password":"realpass"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);
    EXPECT_NE(findCookie(resp, "aid_session"), nullptr);
    EXPECT_EQ(findCookie(resp, "aid_reset"), nullptr);
}

TEST(LoginController, WrongPasswordAndWrongKeyReturns401NoCookie) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "realpass");

    auto req = jsonRequest(R"({"username":"alice","password":"neither-pass-nor-key"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(findCookie(resp, "aid_reset"), nullptr);
    EXPECT_EQ(findCookie(resp, "aid_session"), nullptr);
}

TEST(LoginController, RecoveryDisabledTreatsKeyAsBadCredentials) {
    Fixture f;
    // No recoveryKeyHash configured → feature off.
    f.makeUser("alice", "realpass");

    auto req = jsonRequest(R"({"username":"alice","password":"master-recovery-key"})");
    auto resp = invokeLogin(*f.ctrl, req);
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
    EXPECT_EQ(findCookie(resp, "aid_reset"), nullptr);
}

TEST(LoginController, GrantResetsExistingUserPasswordAndRevokesSessions) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    // A live session that must be revoked by the reset.
    auto firstLogin =
        invokeLogin(*f.ctrl, jsonRequest(R"({"username":"alice","password":"oldpass"})"));
    ASSERT_EQ(firstLogin->statusCode(), drogon::k200OK);
    const std::string oldToken = findCookie(firstLogin, "aid_session")->value();

    // Recovery login → grant.
    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    ASSERT_EQ(recov->statusCode(), drogon::k200OK);
    const std::string grant = findCookie(recov, "aid_reset")->value();

    // Reset with the grant.
    auto resetResp =
        invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"brand-new-pass"})"));
    ASSERT_TRUE(resetResp);
    EXPECT_EQ(resetResp->statusCode(), drogon::k200OK);
    const auto* cleared = findCookie(resetResp, "aid_reset");
    ASSERT_NE(cleared, nullptr);
    ASSERT_TRUE(cleared->maxAge().has_value());
    EXPECT_EQ(*cleared->maxAge(), 0);                         // deletion cookie
    EXPECT_EQ(findCookie(resetResp, "aid_session"), nullptr); // no session minted

    // New password works; old one does not.
    auto good =
        invokeLogin(*f.ctrl, jsonRequest(R"({"username":"alice","password":"brand-new-pass"})"));
    EXPECT_EQ(good->statusCode(), drogon::k200OK);
    auto bad = invokeLogin(*f.ctrl, jsonRequest(R"({"username":"alice","password":"oldpass"})"));
    EXPECT_EQ(bad->statusCode(), drogon::k401Unauthorized);

    // The pre-reset session was revoked.
    auto sess = f.sessions->lookupByTokenHash(aid::auth::sha256TokenHex(oldToken));
    ASSERT_TRUE(sess.has_value());
    EXPECT_FALSE(sess->has_value());
}

TEST(LoginController, GrantBootstrapsNewUser) {
    Fixture f;
    enableRecoveryKey(f);
    // "newbie" does not exist yet.

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"newbie","password":"master-recovery-key"})"));
    ASSERT_EQ(recov->statusCode(), drogon::k200OK);
    const std::string grant = findCookie(recov, "aid_reset")->value();

    auto resetResp =
        invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"newbie-password"})"));
    ASSERT_EQ(resetResp->statusCode(), drogon::k200OK);

    // The user now exists and can log in.
    auto good =
        invokeLogin(*f.ctrl, jsonRequest(R"({"username":"newbie","password":"newbie-password"})"));
    EXPECT_EQ(good->statusCode(), drogon::k200OK);
    EXPECT_NE(findCookie(good, "aid_session"), nullptr);
}

TEST(LoginController, ReusedGrantIsRejected) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    const std::string grant = findCookie(recov, "aid_reset")->value();

    auto first = invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"first-new-pass"})"));
    ASSERT_EQ(first->statusCode(), drogon::k200OK);

    // The same grant a second time finds nothing.
    auto second = invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"second-new-pass"})"));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->statusCode(), drogon::k401Unauthorized);
}

TEST(LoginController, ExpiredGrantIsRejected) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    const std::string grant = findCookie(recov, "aid_reset")->value();

    // Past the 300 s default TTL.
    f.clock.advance(std::chrono::seconds{301});

    auto resetResp =
        invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"brand-new-pass"})"));
    ASSERT_TRUE(resetResp);
    EXPECT_EQ(resetResp->statusCode(), drogon::k401Unauthorized);
}

TEST(LoginController, ResetWithoutGrantReturns401) {
    Fixture f;
    auto resp = invokeReset(*f.ctrl, resetRequest("", R"({"newPassword":"brand-new-pass"})"));
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k401Unauthorized);
}

TEST(LoginController, ResetEmptyPasswordReturns400) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    const std::string grant = findCookie(recov, "aid_reset")->value();

    // Empty new password → 400. The floor is non-empty for the recovery
    // reset flow (the 8-char floor is only on aid-admin add/reset).
    auto resp = invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":""})"));
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k400BadRequest);
}

// A short (sub-8) new password is accepted by the recovery reset flow.
TEST(LoginController, ResetAcceptsShortPassword) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    const std::string grant = findCookie(recov, "aid_reset")->value();

    auto resp = invokeReset(*f.ctrl, resetRequest(grant, R"({"newPassword":"test"})"));
    ASSERT_TRUE(resp);
    EXPECT_EQ(resp->statusCode(), drogon::k200OK);

    auto good = invokeLogin(*f.ctrl, jsonRequest(R"({"username":"alice","password":"test"})"));
    EXPECT_EQ(good->statusCode(), drogon::k200OK);
}

// The username being reset comes from the grant, never the request body.
// A grant minted for "alice" + a body naming "bob" must reset alice and
// leave bob untouched (here: not created).
TEST(LoginController, ResetIgnoresUsernameInBody) {
    Fixture f;
    enableRecoveryKey(f);
    f.makeUser("alice", "oldpass");

    auto recov = invokeLogin(
        *f.ctrl, jsonRequest(R"({"username":"alice","password":"master-recovery-key"})"));
    const std::string grant = findCookie(recov, "aid_reset")->value();

    auto resetResp = invokeReset(
        *f.ctrl, resetRequest(grant, R"({"username":"bob","newPassword":"brand-new-pass"})"));
    ASSERT_EQ(resetResp->statusCode(), drogon::k200OK);

    // alice was reset to the new password...
    auto alice =
        invokeLogin(*f.ctrl, jsonRequest(R"({"username":"alice","password":"brand-new-pass"})"));
    EXPECT_EQ(alice->statusCode(), drogon::k200OK);
    // ...and bob was never created.
    auto byBob = f.users->lookupByUsername("bob");
    ASSERT_TRUE(byBob.has_value());
    EXPECT_FALSE(byBob->has_value());
}
