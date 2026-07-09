#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>

#include <functional>

namespace aid::auth {
class AuthService;
class ResetGrantStore;
} // namespace aid::auth

namespace aid::crosscutting {
class CorrelationId;
class Logger;
struct AuthConfig;
} // namespace aid::crosscutting

namespace aid::controllers {

// Drogon HTTP handler for the auth endpoints:
//   POST /ui/login   — exempt from SessionGuard (this is how you get a session)
//   POST /ui/reset    — exempt from SessionGuard (gated by a single-use reset grant cookie)
//   POST /ui/logout  — behind SessionGuard
//   GET  /ui/whoami  — behind SessionGuard
//
// Owns cookie shaping, JSON I/O, and HTTP-status mapping; no business
// logic — everything goes through AuthService / ResetGrantStore.
class LoginController {
public:
    LoginController(aid::auth::AuthService& auth, aid::auth::ResetGrantStore& grants,
                    aid::crosscutting::Logger& logger, aid::crosscutting::CorrelationId& cid,
                    const aid::crosscutting::AuthConfig& cfg);

    LoginController(const LoginController&) = delete;
    LoginController& operator=(const LoginController&) = delete;
    LoginController(LoginController&&) = delete;
    LoginController& operator=(LoginController&&) = delete;
    ~LoginController() = default;

    void postLogin(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    // POST /ui/reset — set a new password using the single-use grant in
    // the `aid_reset` cookie (no session required). The username comes
    // from the consumed grant; the body carries only the new password.
    void postReset(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void postLogout(const drogon::HttpRequestPtr& req,
                    std::function<void(const drogon::HttpResponsePtr&)>&& callback);

    void getWhoami(const drogon::HttpRequestPtr& req,
                   std::function<void(const drogon::HttpResponsePtr&)>&& callback);

private:
    aid::auth::AuthService& auth_;
    aid::auth::ResetGrantStore& grants_;
    aid::crosscutting::Logger& logger_;
    aid::crosscutting::CorrelationId& cid_;
    const aid::crosscutting::AuthConfig& cfg_;
};

} // namespace aid::controllers
