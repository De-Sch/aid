#pragma once

#include <drogon/HttpFilter.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/drogon_callbacks.h>

#include <string>
#include <string_view>

namespace aid::auth {
class UserRepo;
class SessionRepo;
} // namespace aid::auth

namespace aid::crosscutting {
class Clock;
class Logger;
struct AuthConfig;
} // namespace aid::crosscutting

namespace aid::controllers {

// Drogon HttpFilter that gates /ui/* (except /ui/login) on a valid
// session cookie. Reads cfg.cookieName from the request, resolves it
// to a UserHandle via SessionRepo + UserRepo, slides the session
// expiry, and attaches the handle to the request attributes under
// VIEWER_KEY. Fails closed on any error path — 401, never 500.
//
// AutoCreation=false: this filter has a non-default constructor and is
// installed manually in Main via app().registerFilter(make_shared(...)).
class SessionGuard : public drogon::HttpFilter<SessionGuard, false> {
public:
    // The request-attribute key under which the resolved UserHandle is
    // stored for downstream controllers. The string spelling is the
    // contract; consumers MUST read via VIEWER_KEY.
    //
    // Convention: if the attribute is absent (e.g. a route not behind
    // SessionGuard reached the controller anyway), `attributes()->get<UserHandle>`
    // returns a default-constructed `UserHandle` with an empty `v`.
    // LoginController::getWhoami uses that as the "fail closed" signal.
    static inline const std::string VIEWER_KEY{"viewer"};

    SessionGuard(aid::auth::UserRepo& users, aid::auth::SessionRepo& sessions,
                 aid::crosscutting::Clock& clock, aid::crosscutting::Logger& logger,
                 const aid::crosscutting::AuthConfig& cfg) noexcept;

    SessionGuard(const SessionGuard&) = delete;
    SessionGuard& operator=(const SessionGuard&) = delete;
    SessionGuard(SessionGuard&&) = delete;
    SessionGuard& operator=(SessionGuard&&) = delete;
    ~SessionGuard() override = default;

    void doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                  drogon::FilterChainCallback&& fccb) override;

private:
    aid::auth::UserRepo& users_;
    aid::auth::SessionRepo& sessions_;
    aid::crosscutting::Clock& clock_;
    aid::crosscutting::Logger& logger_;
    const aid::crosscutting::AuthConfig& cfg_;
};

} // namespace aid::controllers
