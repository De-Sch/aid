#include "aid/controllers/LoginController.h"

#include <drogon/Cookie.h>
#include <drogon/HttpTypes.h>

#include <algorithm>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/auth/AuthService.h"
#include "aid/auth/ResetGrantStore.h"
#include "aid/controllers/ControllerSupport.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/Error.h"
#include "aid/value-types/Ids.h"

namespace aid::controllers {

using aid::crosscutting::LogType;
using aid::plumbing::ErrorCode;

namespace {

// Distinct from cfg.cookieName (the session cookie) on purpose: a reset
// grant must never be accepted as a session by SessionGuard, nor a
// session as a grant. Lifetime matches ResetGrantStore's default TTL.
constexpr const char* kResetCookieName = "aid_reset";
constexpr int kResetCookieMaxAge = 300;

// Minimum new-password length for the recovery-reset flow. Relaxed to
// non-empty (the recovery path serves an internal, trusted tool);
// add-user / reset-password keep the
// 8-char floor in aid-admin. The daemon still validates server-side
// rather than trusting the SPA.
constexpr std::size_t kMinPasswordLen = 1;

[[nodiscard]] std::optional<std::string> stringField(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

// Resolve the client's IP for the `sessions.ip_at_login` audit column.
// `X-Forwarded-For` is honored only when the operator has explicitly
// turned on `cfg.trustForwardedFor` AND the peer address is in
// `cfg.trustedProxyAddresses`. Otherwise the raw peer address wins —
// an unauthenticated client otherwise forges audit data by sending
// the header itself.
[[nodiscard]] std::string clientIp(const drogon::HttpRequestPtr& req,
                                   const aid::crosscutting::AuthConfig& cfg) {
    const std::string peer = req->getPeerAddr().toIp();
    if (!cfg.trustForwardedFor) {
        return peer;
    }
    const auto& proxies = cfg.trustedProxyAddresses;
    if (std::find(proxies.begin(), proxies.end(), peer) == proxies.end()) {
        return peer;
    }
    const auto& xff = req->getHeader("X-Forwarded-For");
    if (xff.empty()) {
        return peer;
    }
    const auto comma = xff.find(',');
    return std::string{xff.substr(0, comma == std::string::npos ? xff.size() : comma)};
}

[[nodiscard]] drogon::Cookie buildSessionCookie(const aid::crosscutting::AuthConfig& cfg,
                                                std::string_view token) {
    drogon::Cookie c{cfg.cookieName, std::string{token}};
    c.setHttpOnly(true);
    c.setSecure(cfg.cookieSecure);
    c.setSameSite(drogon::Cookie::SameSite::kStrict);
    c.setPath("/");
    c.setMaxAge(cfg.sessionLifetimeSeconds);
    return c;
}

// Truncates `s` to at most `maxBytes` bytes, walking back past any
// trailing partial UTF-8 sequence so we don't store a half-encoded
// code point. If the input is invalid UTF-8 we drop whatever trails;
// the bytes preceding that are kept as-is.
void truncateToUtf8Boundary(std::string& s, std::size_t maxBytes) {
    if (s.size() <= maxBytes) {
        return;
    }
    s.resize(maxBytes);
    auto i = s.size();
    int continuations = 0;
    while (continuations < 3 && i > 0 && (static_cast<unsigned char>(s[i - 1]) & 0xC0) == 0x80) {
        --i;
        ++continuations;
    }
    if (i == 0) {
        s.clear();
        return;
    }
    const auto lead = static_cast<unsigned char>(s[i - 1]);
    int expected = 0;
    if ((lead & 0x80) == 0x00) {
        expected = 0;
    } else if ((lead & 0xE0) == 0xC0) {
        expected = 1;
    } else if ((lead & 0xF0) == 0xE0) {
        expected = 2;
    } else if ((lead & 0xF8) == 0xF0) {
        expected = 3;
    } else {
        // Invalid leading byte — drop it.
        s.resize(i - 1);
        return;
    }
    if (continuations < expected) {
        // Incomplete sequence — drop the lead too.
        s.resize(i - 1);
    }
}

[[nodiscard]] drogon::Cookie buildExpiredCookie(const aid::crosscutting::AuthConfig& cfg) {
    drogon::Cookie c{cfg.cookieName, ""};
    // Match the attributes of the original cookie. Firefox/Safari
    // reject deletion Set-Cookie headers whose Secure / SameSite don't
    // match what was set originally, leaving the stale cookie in the
    // browser's jar even after the server has revoked the row.
    c.setHttpOnly(true);
    c.setSecure(cfg.cookieSecure);
    c.setSameSite(drogon::Cookie::SameSite::kStrict);
    c.setPath("/");
    c.setMaxAge(0);
    return c;
}

// Carries the single-use reset grant. Same hardening as the session
// cookie (HttpOnly + Secure + SameSite=Strict) but a short Max-Age that
// matches the grant TTL, so a stale grant cookie expires client-side too.
[[nodiscard]] drogon::Cookie buildResetCookie(const aid::crosscutting::AuthConfig& cfg,
                                              std::string_view grant) {
    drogon::Cookie c{kResetCookieName, std::string{grant}};
    c.setHttpOnly(true);
    c.setSecure(cfg.cookieSecure);
    c.setSameSite(drogon::Cookie::SameSite::kStrict);
    c.setPath("/");
    c.setMaxAge(kResetCookieMaxAge);
    return c;
}

[[nodiscard]] drogon::Cookie buildExpiredResetCookie(const aid::crosscutting::AuthConfig& cfg) {
    drogon::Cookie c{kResetCookieName, ""};
    c.setHttpOnly(true);
    c.setSecure(cfg.cookieSecure);
    c.setSameSite(drogon::Cookie::SameSite::kStrict);
    c.setPath("/");
    c.setMaxAge(0);
    return c;
}

} // namespace

LoginController::LoginController(aid::auth::AuthService& auth, aid::auth::ResetGrantStore& grants,
                                 aid::crosscutting::Logger& logger,
                                 aid::crosscutting::CorrelationId& cid,
                                 const aid::crosscutting::AuthConfig& cfg)
    : auth_(auth), grants_(grants), logger_(logger), cid_(cid), cfg_(cfg) {
}

void LoginController::postLogin(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string cidStr = cid_.nextUuid();

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        // No body in the log line — never echo the raw request body, may contain a password.
        logger_.warn("LoginController: malformed login body", LogType::FRONTEND, cidStr);
        callback(jsonResponse(drogon::k400BadRequest, R"({"error":"bad request"})"));
        return;
    }
    auto usernameOpt = stringField(j, "username");
    auto passwordOpt = stringField(j, "password");
    if (!usernameOpt || usernameOpt->empty() || !passwordOpt || passwordOpt->empty()) {
        callback(jsonResponse(drogon::k400BadRequest, R"({"error":"bad request"})"));
        return;
    }

    // Cap audit-column inputs to a sane length before they reach the
    // DB — SQLite has no per-column size enforcement, and an attacker
    // can otherwise sustain large User-Agent / X-Forwarded-For strings
    // to balloon auth.db. truncateToUtf8Boundary snaps back past any
    // partial trailing sequence so we don't store a half-encoded code
    // point that would later trip up JSON serialization in admin tools.
    constexpr std::size_t kAuditFieldMax = 256;
    std::string ip = clientIp(req, cfg_);
    truncateToUtf8Boundary(ip, kAuditFieldMax);
    std::string ua{req->getHeader("User-Agent")};
    truncateToUtf8Boundary(ua, kAuditFieldMax);

    auto task = auth_.login(*usernameOpt, *passwordOpt, ip, ua);
    if (!task.done()) {
        // AuthService bodies have no co_await — this never trips. If it
        // does, fail closed: don't leave the request hanging.
        logger_.error("LoginController: login task suspended unexpectedly", LogType::FRONTEND,
                      cidStr);
        callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
        return;
    }
    auto r = task.await_resume();
    if (!r) {
        switch (r.error().code) {
        case ErrorCode::Unauthenticated: {
            // Normal credentials failed. Before rejecting, check whether
            // the supplied password is the recovery key — if so, this is
            // a password reset / bootstrap, not a login: issue a
            // single-use grant and steer the client to the reset page.
            // tryRecoveryKey is a no-op (Ok(false)) when the feature is
            // disabled, so this path stays free on a daemon with no
            // recovery key configured.
            auto rkTask = auth_.tryRecoveryKey(*passwordOpt);
            if (!rkTask.done()) {
                logger_.error("LoginController: recovery task suspended unexpectedly",
                              LogType::FRONTEND, cidStr);
                callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
                return;
            }
            auto rk = rkTask.await_resume();
            if (!rk) {
                if (rk.error().code == ErrorCode::TooManyRequests) {
                    callback(jsonResponse(drogon::k429TooManyRequests,
                                          R"({"error":"too many requests"})"));
                    return;
                }
                logger_.error("LoginController: recovery check failed: " + rk.error().message,
                              LogType::FRONTEND, cidStr);
                callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
                return;
            }
            if (*rk) {
                const std::string grant = grants_.issue(*usernameOpt);
                auto resp = jsonResponse(drogon::k200OK, R"({"resetRequired":true})");
                resp->addCookie(buildResetCookie(cfg_, grant));
                callback(resp);
                return;
            }
            // Not the recovery key either → genuine bad credentials.
            callback(jsonResponse(drogon::k401Unauthorized, R"({"error":"invalid credentials"})"));
            return;
        }
        case ErrorCode::TooManyRequests:
            callback(jsonResponse(drogon::k429TooManyRequests, R"({"error":"too many requests"})"));
            return;
        default:
            logger_.error("LoginController: login failed: " + r.error().message, LogType::FRONTEND,
                          cidStr);
            callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
            return;
        }
    }

    auto resp = jsonResponse(drogon::k200OK, R"({"ok":true})");
    resp->addCookie(buildSessionCookie(cfg_, r->token.v));
    callback(resp);
}

void LoginController::postReset(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string cidStr = cid_.nextUuid();

    // The grant cookie is the only authorization for this endpoint
    // (it is exempt from SessionGuard). No cookie → nothing to reset.
    const std::string& grant = req->getCookie(kResetCookieName);
    if (grant.empty()) {
        callback(jsonResponse(drogon::k401Unauthorized, R"({"error":"no reset grant"})"));
        return;
    }

    nlohmann::json j;
    try {
        j = nlohmann::json::parse(req->getBody());
    } catch (const std::exception&) {
        // Never echo the body — it carries a plaintext password.
        logger_.warn("LoginController: malformed reset body", LogType::FRONTEND, cidStr);
        callback(jsonResponse(drogon::k400BadRequest, R"({"error":"bad request"})"));
        return;
    }
    auto newPasswordOpt = stringField(j, "newPassword");
    if (!newPasswordOpt || newPasswordOpt->size() < kMinPasswordLen) {
        callback(jsonResponse(drogon::k400BadRequest, R"({"error":"weak password"})"));
        return;
    }

    // One-time consume: the grant is erased here, before the reset is
    // applied, so a replayed cookie finds nothing. The username is taken
    // ONLY from the grant — never from the request body — so a caller
    // cannot reset an account it did not name at /ui/login.
    auto userOpt = grants_.consume(grant);
    if (!userOpt) {
        callback(jsonResponse(drogon::k401Unauthorized, R"({"error":"invalid or expired grant"})"));
        return;
    }

    auto task = auth_.applyReset(userOpt->v, *newPasswordOpt);
    if (!task.done()) {
        logger_.error("LoginController: reset task suspended unexpectedly", LogType::FRONTEND,
                      cidStr);
        callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
        return;
    }
    auto r = task.await_resume();
    if (!r) {
        switch (r.error().code) {
        case ErrorCode::InvalidInput:
            callback(jsonResponse(drogon::k400BadRequest, R"({"error":"weak password"})"));
            return;
        case ErrorCode::Conflict:
            // The username was created by a racing request between the
            // grant being issued and consumed. Rare; the operator can
            // just restart from /ui/login.
            callback(jsonResponse(drogon::k409Conflict, R"({"error":"conflict"})"));
            return;
        case ErrorCode::TooManyRequests:
            callback(jsonResponse(drogon::k429TooManyRequests, R"({"error":"too many requests"})"));
            return;
        default:
            logger_.error("LoginController: reset failed: " + r.error().message, LogType::FRONTEND,
                          cidStr);
            callback(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
            return;
        }
    }

    // No session is minted by the reset flow — force a fresh /ui/login.
    auto resp = jsonResponse(drogon::k200OK, R"({"ok":true})");
    resp->addCookie(buildExpiredResetCookie(cfg_));
    callback(resp);
}

void LoginController::postLogout(const drogon::HttpRequestPtr& req,
                                 std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string& cookie = req->getCookie(cfg_.cookieName);
    if (!cookie.empty()) {
        auto task = auth_.logout(aid::auth::SessionToken{cookie});
        if (task.done()) {
            (void)task.await_resume();
        }
    }
    auto resp = jsonResponse(drogon::k200OK, R"({"ok":true})");
    resp->addCookie(buildExpiredCookie(cfg_));
    callback(resp);
}

void LoginController::getWhoami(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const auto& viewer = req->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
    if (viewer.v.empty()) {
        // SessionGuard always sets the viewer for authenticated routes;
        // an empty handle indicates the controller was reached without
        // the filter — fail closed.
        callback(jsonResponse(drogon::k401Unauthorized, R"({"error":"unauthenticated"})"));
        return;
    }
    nlohmann::json body;
    body["username"] = viewer.v;
    callback(jsonResponse(drogon::k200OK, body.dump()));
}

} // namespace aid::controllers
