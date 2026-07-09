#pragma once

#include <optional>
#include <semaphore>
#include <string>
#include <string_view>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace aid::crosscutting {
class Clock;
struct AuthConfig;
} // namespace aid::crosscutting

namespace aid::auth {

class UserRepo;
class SessionRepo;

// Compile-time ceiling on the login-concurrency semaphore. cfg.maxConcurrentLogins
// is initialized into a std::counting_semaphore<kMaxLoginConcurrencyCap> at
// construction; values above the cap are clipped. The cap is well above any
// realistic operator deployment (the Argon2 cost makes anything above ~32 a
// terrible memory budget).
inline constexpr std::ptrdiff_t kMaxLoginConcurrencyCap = 256;

// Plaintext session token — 64-char hex of 32 random bytes. Exists on
// the server side only inside LoginResult (returned exactly once) and
// in the client's cookie. The DB stores SHA-256 hash + 8-char prefix.
struct SessionTokenTag;
using SessionToken = aid::Id<SessionTokenTag>;

struct LoginResult {
    SessionToken token;
    std::optional<std::string> ipAtLogin;
    std::optional<std::string> userAgent;
    aid::Timestamp expiresAt;
};

// Hex SHA-256 of a session token's plaintext value. Exposed so that
// SessionGuard (in aid_controllers) can produce the same hash from a
// cookie value without re-linking libsodium itself.
[[nodiscard]] std::string sha256TokenHex(std::string_view in);

// 32 random bytes hex-encoded with libsodium's constant-time encoder
// (64 hex chars). Exposed so ResetGrantStore can mint grant tokens with
// the same entropy + encoding as session tokens without re-linking
// libsodium. Calls PasswordHasher::initialize() internally.
[[nodiscard]] std::string mintTokenHex();

// AuthService stitches UserRepo + SessionRepo + PasswordHasher together
// for the three auth flows (login, logout, whoami). Pure orchestration:
// no JSON, no HTTP types — those live in LoginController.
//
// All methods return Task<Result<T>> for uniformity with the rest of
// the daemon. The bodies are synchronous SQLite + libsodium calls; the
// coroutine wrapper costs one suspend/resume per call.
class AuthService {
public:
    AuthService(UserRepo& users, SessionRepo& sessions, aid::crosscutting::Clock& clock,
                const aid::crosscutting::AuthConfig& cfg) noexcept;

    AuthService(const AuthService&) = delete;
    AuthService& operator=(const AuthService&) = delete;
    AuthService(AuthService&&) = delete;
    AuthService& operator=(AuthService&&) = delete;
    ~AuthService() = default;

    // Step-for-step login flow. Returns
    // Unauthenticated on bad credentials (no user-enumeration leak;
    // timing-equal via dummy-hash verify), TooManyRequests when the
    // login-concurrency cap is already saturated (memory-DoS guard).
    // No auto-lockout.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<LoginResult>>
    login(std::string_view username, std::string_view password, std::string_view ipAtLogin,
          std::string_view userAgent);

    // Hash → SessionRepo::revoke. Idempotent: logging out an
    // already-revoked or never-existing token still returns success.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    logout(const SessionToken& token);

    // Hash → prefix lookup → verify hash → expiry check → UserHandle.
    // No side effect on the session row — SessionGuard owns sliding.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::UserHandle>>
    whoami(const SessionToken& token);

    // Recovery key (master password). Returns Ok(true) iff `candidate`
    // Argon2id-verifies against cfg_.recoveryKeyHash, Ok(false) on any
    // miss, Err(TooManyRequests) when the login-concurrency cap is
    // saturated. Returns Ok(false) WITHOUT any Argon2 work when the
    // feature is off (cfg_.recoveryKeyHash == std::nullopt) or the
    // candidate is empty. Touches no repo — the recovery key is global,
    // independent of whether any `username` exists. Shares login()'s
    // concurrency semaphore so the recovery path cannot bypass the
    // memory-DoS ceiling.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<bool>>
    tryRecoveryKey(std::string_view candidate);

    // Applies a verified password reset for `username`. The caller (the
    // reset controller) passes the username it obtained from a consumed
    // single-use grant — AuthService never sees the grant itself. If the
    // user exists: Argon2id-hash newPassword, setPasswordHash, then
    // revoke all that user's sessions. If not: create the user (this is
    // the first-user bootstrap path). Returns InvalidInput on empty
    // newPassword, Conflict only on a create race, TooManyRequests when
    // the concurrency cap is saturated.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    applyReset(std::string_view username, std::string_view newPassword);

private:
    UserRepo& users_;
    SessionRepo& sessions_;
    aid::crosscutting::Clock& clock_;
    const aid::crosscutting::AuthConfig& cfg_;
    // Caps concurrent Argon2id verifies in login() to bound the per-process
    // RAM footprint of /ui/login under attack. try_acquire is non-blocking,
    // so the Drogon event loop never stalls on the semaphore — overflow
    // surfaces as ErrorCode::TooManyRequests instead.
    std::counting_semaphore<kMaxLoginConcurrencyCap> loginSem_;
};

} // namespace aid::auth
