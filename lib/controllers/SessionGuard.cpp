#include "aid/controllers/SessionGuard.h"

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <string>
#include <string_view>
#include <utility>

#include "aid/auth/AuthService.h" // for sha256TokenHex
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"
#include "aid/crosscutting/Logger.h"

namespace aid::controllers {

namespace {

constexpr std::size_t kTokenHexLen = 64;

[[nodiscard]] drogon::HttpResponsePtr unauthorized() {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(drogon::k401Unauthorized);
    return resp;
}

} // namespace

SessionGuard::SessionGuard(aid::auth::UserRepo& users, aid::auth::SessionRepo& sessions,
                           aid::crosscutting::Clock& clock, aid::crosscutting::Logger& logger,
                           const aid::crosscutting::AuthConfig& cfg) noexcept
    : users_(users), sessions_(sessions), clock_(clock), logger_(logger), cfg_(cfg) {
}

void SessionGuard::doFilter(const drogon::HttpRequestPtr& req, drogon::FilterCallback&& fcb,
                            drogon::FilterChainCallback&& fccb) {
    // Step 1: cookie present + well-formed?
    const std::string& token = req->getCookie(cfg_.cookieName);
    if (token.size() != kTokenHexLen) {
        fcb(unauthorized());
        return;
    }

    // Step 2: full hash of the plaintext for the UNIQUE-indexed lookup.
    const std::string tokenHash = aid::auth::sha256TokenHex(token);

    // Step 3: token_hash lookup. The UNIQUE index guarantees at most
    // one match; an unknown token is the only "no match" path.
    auto sess = sessions_.lookupByTokenHash(tokenHash);
    if (!sess) {
        logger_.warn("session lookup failed: " + sess.error().message,
                     aid::crosscutting::LogType::FRONTEND);
        fcb(unauthorized());
        return;
    }
    if (!sess->has_value()) {
        fcb(unauthorized());
        return;
    }

    // Step 4: expiry — opportunistically revoke and reject.
    if ((*sess)->expiresAt <= clock_.now()) {
        (void)sessions_.revoke((*sess)->tokenHash);
        fcb(unauthorized());
        return;
    }

    // Step 5: slide the expiry. Single UPDATE.
    if (auto sl = sessions_.slide((*sess)->id); !sl) {
        logger_.warn("session slide failed: " + sl.error().message,
                     aid::crosscutting::LogType::FRONTEND);
        fcb(unauthorized());
        return;
    }

    // Step 6: resolve the user — guard against race where a user is
    // deleted between session creation and this request.
    auto user = users_.lookupById((*sess)->userId);
    if (!user) {
        logger_.warn("user lookup failed: " + user.error().message,
                     aid::crosscutting::LogType::FRONTEND);
        fcb(unauthorized());
        return;
    }
    if (!user->has_value()) {
        (void)sessions_.revoke((*sess)->tokenHash);
        fcb(unauthorized());
        return;
    }

    // Step 7: attach the viewer handle for downstream controllers.
    req->attributes()->insert(VIEWER_KEY, (*user)->handle);

    // Step 8: pass through.
    fccb();
}

} // namespace aid::controllers
