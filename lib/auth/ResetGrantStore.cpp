#include "aid/auth/ResetGrantStore.h"

#include <chrono>
#include <mutex>
#include <utility>

#include "aid/auth/AuthService.h" // mintTokenHex, sha256TokenHex
#include "aid/crosscutting/Clock.h"

namespace aid::auth {

ResetGrantStore::ResetGrantStore(aid::crosscutting::Clock& clock, int ttlSeconds) noexcept
    : clock_(clock), ttlSeconds_(ttlSeconds) {
}

std::string ResetGrantStore::issue(std::string_view username) {
    const std::string token = mintTokenHex();
    const std::string tokenHash = sha256TokenHex(token);
    const aid::Timestamp expiresAt = clock_.now() + std::chrono::seconds{ttlSeconds_};

    const std::lock_guard<std::mutex> lock{mu_};
    // Sweep expired entries on every insert. The map is keyed by the
    // operator's own login attempts so it is tiny in practice, but this
    // keeps an abandoned-grant accumulation from ever growing without
    // bound.
    const aid::Timestamp now = clock_.now();
    for (auto it = byHash_.begin(); it != byHash_.end();) {
        it = (it->second.expiresAt <= now) ? byHash_.erase(it) : std::next(it);
    }
    byHash_[tokenHash] = Entry{std::string{username}, expiresAt};
    return token;
}

std::optional<aid::UserHandle> ResetGrantStore::consume(std::string_view plaintextToken) {
    if (plaintextToken.empty()) {
        return std::nullopt;
    }
    const std::string tokenHash = sha256TokenHex(plaintextToken);

    const std::lock_guard<std::mutex> lock{mu_};
    auto it = byHash_.find(tokenHash);
    if (it == byHash_.end()) {
        return std::nullopt;
    }
    // Erase regardless of expiry — a found grant is spent the moment it
    // is looked up (one-time use; a replay finds nothing).
    aid::UserHandle handle{std::move(it->second.username)};
    const aid::Timestamp expiresAt = it->second.expiresAt;
    byHash_.erase(it);

    if (expiresAt <= clock_.now()) {
        return std::nullopt;
    }
    return handle;
}

} // namespace aid::auth
