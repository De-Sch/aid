#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

#include "aid/value-types/Ids.h"

namespace aid::crosscutting {
class Clock;
} // namespace aid::crosscutting

namespace aid::auth {

// Thread-safe, in-memory store of short-lived single-use "reset grants".
//
// A grant authorizes resetting the password of EXACTLY ONE username. It
// is minted when AuthService verifies the recovery key on /ui/login, and
// consumed when /ui/reset sets the new password. The plaintext grant
// token lives only in the operator's `aid_reset` cookie; this store
// keeps just its SHA-256 hash (same hygiene as session tokens — the raw
// token is never held server-side), the bound username, and an expiry.
//
// In-memory (not SQLite) on purpose: a grant is ephemeral (~5 min) and
// single-use, so surviving a daemon restart has negative value — losing
// in-flight grants on restart is the desirable failure mode for a
// security artifact, and it avoids a schema migration.
class ResetGrantStore {
public:
    // ttlSeconds is the grant lifetime; the default matches the
    // `aid_reset` cookie Max-Age in LoginController. Injectable so tests
    // can use a short TTL (or a FakeClock) to exercise expiry.
    explicit ResetGrantStore(aid::crosscutting::Clock& clock, int ttlSeconds = 300) noexcept;

    ResetGrantStore(const ResetGrantStore&) = delete;
    ResetGrantStore& operator=(const ResetGrantStore&) = delete;
    ResetGrantStore(ResetGrantStore&&) = delete;
    ResetGrantStore& operator=(ResetGrantStore&&) = delete;
    ~ResetGrantStore() = default;

    // Mint a grant for `username`. Returns the PLAINTEXT grant token
    // (64-char hex) for the caller to set as the `aid_reset` cookie.
    // Stores only sha256(token) → {username, now + ttl}. Opportunistically
    // sweeps expired entries so the map can't grow unbounded.
    [[nodiscard]] std::string issue(std::string_view username);

    // One-time consume. Hashes `plaintextToken`, looks it up, and on a
    // live (unexpired) hit ERASES the entry and returns the bound
    // username. Returns std::nullopt for an unknown, expired, or
    // already-consumed token — the three are indistinguishable to the
    // caller by design. The grant is erased before the caller applies
    // the reset, so a replayed token is dead after the first call.
    [[nodiscard]] std::optional<aid::UserHandle> consume(std::string_view plaintextToken);

private:
    struct Entry {
        std::string username;
        aid::Timestamp expiresAt;
    };

    aid::crosscutting::Clock& clock_;
    int ttlSeconds_;
    std::mutex mu_;
    std::unordered_map<std::string, Entry> byHash_; // key = sha256TokenHex(token)
};

} // namespace aid::auth
