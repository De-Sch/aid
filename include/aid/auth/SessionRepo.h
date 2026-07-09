#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/value-types/Ids.h"

namespace aid::crosscutting {
class Clock;
struct AuthConfig;
} // namespace aid::crosscutting

namespace aid::auth {

class AuthDb;

// SessionRepo owns all reads and writes against the `sessions` table.
// The sliding-expiry policy is hardcoded against cfg_.sessionLifetimeSeconds;
// lookups go via the UNIQUE token_hash column directly.
//
// The plaintext token is never stored here. Callers (the login flow)
// hash it before calling create and before any lookup.
class SessionRepo {
public:
    struct Session {
        std::int64_t id;
        std::int64_t userId;
        std::string tokenHash; // SHA-256 hex of the plaintext token
        std::string prefix;    // first 8 chars of the plaintext token
        aid::Timestamp createdAt;
        aid::Timestamp expiresAt;
        aid::Timestamp lastSeenAt;
        std::optional<std::string> ipAtLogin;
        std::optional<std::string> userAgent;
    };

    SessionRepo(AuthDb& db, aid::crosscutting::Clock& clock,
                const aid::crosscutting::AuthConfig& cfg) noexcept;

    SessionRepo(const SessionRepo&) = delete;
    SessionRepo& operator=(const SessionRepo&) = delete;
    SessionRepo(SessionRepo&&) = delete;
    SessionRepo& operator=(SessionRepo&&) = delete;
    ~SessionRepo() = default;

    // INSERT with created_at = last_seen_at = now, expires_at = now +
    // cfg.sessionLifetimeSeconds. Returns the freshly-stored row.
    [[nodiscard]] aid::plumbing::Result<Session>
    create(std::int64_t userId, std::string_view tokenHash, std::string_view prefix,
           std::string_view ipAtLogin, std::string_view userAgent);

    // Lookup by the UNIQUE-indexed token_hash. Returns the row even if
    // expired — caller (SessionGuard) checks expires_at and revokes on
    // miss. Putting the expiry check in SQL would force two queries.
    //
    // The prior prefix-keyed lookup let collisions silently shadow the
    // legitimate session row: 8 hex chars = 32 bits of entropy, and
    // SELECT-by-prefix returned the first match in implementation-defined
    // order. token_hash already carries UNIQUE so there is no collision
    // to contend with.
    [[nodiscard]] aid::plumbing::Result<std::optional<Session>>
    lookupByTokenHash(std::string_view tokenHash) const;

    // UPDATE sessions SET expires_at = now + cfg.sessionLifetimeSeconds,
    // last_seen_at = now WHERE id = ?. Single UPDATE; no SELECT.
    [[nodiscard]] aid::plumbing::Result<void> slide(std::int64_t sessionId);

    // DELETE by token_hash — used by /ui/logout. Idempotent: deleting
    // a non-existent token still returns success.
    [[nodiscard]] aid::plumbing::Result<void> revoke(std::string_view tokenHash);

    // DELETE all sessions for one user — used by AdminCli or
    // "logout everywhere".
    [[nodiscard]] aid::plumbing::Result<void> revokeAllFor(std::int64_t userId);

    // DELETE FROM sessions WHERE expires_at <= now. Returns rows removed.
    // Called hourly by a Drogon timer in production.
    [[nodiscard]] aid::plumbing::Result<int> prune();

    // Admin: enumerate every session, ordered by id ascending. Used by
    // `aid-admin list-sessions`. The session table tracks active
    // operator logins; size is bounded by user count * concurrent
    // browsers.
    [[nodiscard]] aid::plumbing::Result<std::vector<Session>> listAll() const;

    // Admin: same as listAll() filtered by userId. Used by
    // `aid-admin list-sessions --username <name>` after the CLI
    // resolves the username to an id.
    [[nodiscard]] aid::plumbing::Result<std::vector<Session>>
    listAllForUser(std::int64_t userId) const;

private:
    AuthDb& db_;
    aid::crosscutting::Clock& clock_;
    const aid::crosscutting::AuthConfig& cfg_;
};

} // namespace aid::auth
