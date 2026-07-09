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
} // namespace aid::crosscutting

namespace aid::auth {

class AuthDb;

// UserRepo encapsulates all reads and writes against the `users` table
// of auth.db.
//
// Plaintext passwords never reach this class. The login flow hashes
// before calling create/setPasswordHash, and verifies via
// PasswordHasher before invoking recordSuccessfulLogin.
//
// The schema retains `failed_attempts` and `locked_until` columns from
// v1, but no method writes them anymore — auto-lockout was removed
// per the security review's L7 decision (threat model: trusted network,
// brute-force is out of scope). Both columns will be dropped in a
// future v2 migration; until then they sit at their NOT NULL DEFAULT 0
// / NULL initial values for every row.
class UserRepo {
public:
    struct User {
        std::int64_t id;
        aid::UserHandle handle;   // username; doubles as ticket-system login
        std::string passwordHash; // Argon2id encoded string
        aid::Timestamp createdAt;
        std::optional<aid::Timestamp> lastLoginAt;
    };

    UserRepo(AuthDb& db, aid::crosscutting::Clock& clock) noexcept;

    UserRepo(const UserRepo&) = delete;
    UserRepo& operator=(const UserRepo&) = delete;
    UserRepo(UserRepo&&) = delete;
    UserRepo& operator=(UserRepo&&) = delete;
    ~UserRepo() = default;

    [[nodiscard]] aid::plumbing::Result<std::optional<User>>
    lookupByUsername(std::string_view name) const;

    [[nodiscard]] aid::plumbing::Result<std::optional<User>> lookupById(std::int64_t id) const;

    // Returns the new id. On a UNIQUE constraint violation on
    // users.username, returns Error{Conflict, "username already exists"}.
    [[nodiscard]] aid::plumbing::Result<std::int64_t> create(std::string_view name,
                                                             std::string_view passwordHash);

    [[nodiscard]] aid::plumbing::Result<void> deleteUser(std::int64_t id);

    [[nodiscard]] aid::plumbing::Result<void> setPasswordHash(std::int64_t id,
                                                              std::string_view newHash);

    // last_login_at = now. One UPDATE.
    [[nodiscard]] aid::plumbing::Result<void> recordSuccessfulLogin(std::int64_t id);

    // Admin: enumerate every user, ordered by id ascending. Used by
    // `aid-admin list-users`. The user table is small (operator
    // accounts), so loading everything is fine.
    [[nodiscard]] aid::plumbing::Result<std::vector<User>> listAll() const;

private:
    AuthDb& db_;
    aid::crosscutting::Clock& clock_;
};

} // namespace aid::auth
