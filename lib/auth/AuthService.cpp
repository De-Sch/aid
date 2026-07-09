#include "aid/auth/AuthService.h"

#include <sodium.h>

#include <array>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>

#include "aid/auth/PasswordHasher.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"

namespace aid::auth {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

namespace {

constexpr std::size_t kTokenBytes = 32; // 32 random bytes → 64 hex chars
constexpr std::size_t kTokenHexLen = kTokenBytes * 2;
constexpr std::size_t kPrefixLen = 8;

[[nodiscard]] Error makeError(ErrorCode code, std::string msg) {
    return Error{code, std::move(msg), std::nullopt};
}

} // namespace

// 32 random bytes hex-encoded with libsodium's constant-time encoder.
// Namespace-scope (declared in AuthService.h) so ResetGrantStore reuses
// it; references the file-local kToken* constants in the anonymous
// namespace above (visible throughout this translation unit).
std::string mintTokenHex() {
    PasswordHasher::initialize(); // ensures sodium_init has run
    std::array<unsigned char, kTokenBytes> raw{};
    randombytes_buf(raw.data(), raw.size());

    // sodium_bin2hex writes (2*n)+1 bytes including a trailing NUL.
    std::string hex(kTokenHexLen + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), raw.data(), raw.size());
    hex.resize(kTokenHexLen); // drop the trailing NUL the helper writes
    sodium_memzero(raw.data(), raw.size());
    return hex;
}

std::string sha256TokenHex(std::string_view in) {
    PasswordHasher::initialize();
    std::array<unsigned char, crypto_hash_sha256_BYTES> out{};
    crypto_hash_sha256(out.data(), reinterpret_cast<const unsigned char*>(in.data()), in.size());
    std::string hex(out.size() * 2 + 1, '\0');
    sodium_bin2hex(hex.data(), hex.size(), out.data(), out.size());
    hex.resize(out.size() * 2);
    return hex;
}

namespace {

// Clip cfg.maxConcurrentLogins into [1, kMaxLoginConcurrencyCap]. A
// misconfigured 0 or negative value would turn login() into a
// permanent-429 endpoint; we'd rather log-and-clip than silently brick
// auth on a typo.
[[nodiscard]] std::ptrdiff_t clampConcurrencyCap(int requested) noexcept {
    if (requested < 1) {
        return 1;
    }
    if (requested > kMaxLoginConcurrencyCap) {
        return kMaxLoginConcurrencyCap;
    }
    return static_cast<std::ptrdiff_t>(requested);
}

} // namespace

AuthService::AuthService(UserRepo& users, SessionRepo& sessions, aid::crosscutting::Clock& clock,
                         const aid::crosscutting::AuthConfig& cfg) noexcept
    : users_(users), sessions_(sessions), clock_(clock), cfg_(cfg),
      loginSem_(clampConcurrencyCap(cfg.maxConcurrentLogins)) {
}

Task<Result<LoginResult>> AuthService::login(std::string_view username, std::string_view password,
                                             std::string_view ipAtLogin,
                                             std::string_view userAgent) {
    // Defense-in-depth: an empty password reaches this method only via
    // a buggy caller (LoginController already rejects empty fields
    // with 400). Bail before the user lookup so we don't waste the
    // Argon2 work; return the same Error as bad-creds so this branch
    // can never be probed independently. Note this is NOT timing-equal
    // with the bad-creds path — it returns ~200 ms faster than a real
    // verify — but since the controller filters empty inputs at the
    // edge, no attacker can reach this branch over HTTP to time it.
    if (password.empty()) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "invalid credentials"));
    }

    // Memory-DoS guard: cap concurrent Argon2id verifies. try_acquire
    // is non-blocking so the Drogon event loop never stalls on this —
    // overflow returns 429 immediately. RAII releases on every exit
    // path below (success, bad-creds, DB error, anything).
    //
    // REASON for an RAII guard rather than a manual release: there is
    // NO co_await between this acquire and the end of this function
    // in the as-built body, so the slot is never held across a thread
    // hop. If a future edit introduces a co_await between acquire and
    // release, the slot WILL be held across the suspension — that's
    // valid for std::counting_semaphore (release can happen on a
    // different thread) but means the cap then bounds Argon2 *plus*
    // whatever the await is doing. Revisit the cap default if that
    // happens.
    if (!loginSem_.try_acquire()) {
        co_return unexpected(makeError(ErrorCode::TooManyRequests, "login throttle"));
    }
    struct SemRelease {
        std::counting_semaphore<kMaxLoginConcurrencyCap>& s;
        ~SemRelease() { s.release(); }
    } slotGuard{loginSem_};

    // Step 1: look up the user (may miss).
    auto userOpt = users_.lookupByUsername(username);
    if (!userOpt) {
        co_return unexpected(userOpt.error());
    }

    // Step 2-3: timing-equal verify against either the real hash or the
    // dummy hash. The unknown-username path costs the same wall-clock.
    const std::string& hashToVerify =
        userOpt->has_value() ? (*userOpt)->passwordHash : PasswordHasher::dummyHash();
    const bool ok = PasswordHasher::verify(password, hashToVerify);

    // Step 4: bad creds — return Unauthenticated. No enumeration leak:
    // both the unknown-user and wrong-password branches return the
    // same Error. Per the security review's L7 decision the
    // failed-attempts counter is NOT incremented — auto-lockout was
    // removed (threat model assumes a trusted network; brute-force
    // protection is out of scope).
    if (!userOpt->has_value() || !ok) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "invalid credentials"));
    }

    // Step 5: record success — updates last_login_at.
    if (auto rec = users_.recordSuccessfulLogin((*userOpt)->id); !rec) {
        co_return unexpected(rec.error());
    }

    // Step 6: opportunistic rehash if the stored params are weaker than
    // the current preset. Out-of-memory here is non-fatal — the user
    // logged in fine, the upgrade can retry on a future login.
    if (PasswordHasher::needsRehash((*userOpt)->passwordHash)) {
        if (auto newHash = PasswordHasher::hash(password); newHash) {
            (void)users_.setPasswordHash((*userOpt)->id, *newHash);
        }
    }

    // Step 7: mint token, hash it, persist the session row.
    const std::string token = mintTokenHex();
    const std::string tokenHash = sha256TokenHex(token);
    const std::string prefix = token.substr(0, kPrefixLen);

    auto sessR = sessions_.create((*userOpt)->id, tokenHash, prefix, ipAtLogin, userAgent);
    if (!sessR) {
        co_return unexpected(sessR.error());
    }

    LoginResult out;
    out.token = SessionToken{token};
    if (!ipAtLogin.empty()) {
        out.ipAtLogin = std::string{ipAtLogin};
    }
    if (!userAgent.empty()) {
        out.userAgent = std::string{userAgent};
    }
    out.expiresAt = sessR->expiresAt;
    co_return out;
}

Task<Result<void>> AuthService::logout(const SessionToken& token) {
    // Idempotent on every dimension: empty/short/non-existent token
    // still returns success. SessionRepo::revoke deletes by token_hash
    // and treats "no row matched" as success.
    if (token.v.size() != kTokenHexLen) {
        co_return Result<void>{};
    }
    const std::string tokenHash = sha256TokenHex(token.v);
    auto r = sessions_.revoke(tokenHash);
    if (!r) {
        co_return unexpected(r.error());
    }
    co_return Result<void>{};
}

Task<Result<aid::UserHandle>> AuthService::whoami(const SessionToken& token) {
    if (token.v.size() != kTokenHexLen) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "invalid session"));
    }
    const std::string tokenHash = sha256TokenHex(token.v);

    auto sess = sessions_.lookupByTokenHash(tokenHash);
    if (!sess) {
        co_return unexpected(sess.error());
    }
    // The UNIQUE index guarantees an exact-match row; the column itself
    // is the only equality we need (constant-time compare is for
    // attacker-supplied vs stored, not for the SQL-resolved row).
    if (!sess->has_value()) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "invalid session"));
    }
    if ((*sess)->expiresAt <= clock_.now()) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "session expired"));
    }

    auto user = users_.lookupById((*sess)->userId);
    if (!user) {
        co_return unexpected(user.error());
    }
    if (!user->has_value()) {
        co_return unexpected(makeError(ErrorCode::Unauthenticated, "user no longer exists"));
    }
    co_return (*user)->handle;
}

Task<Result<bool>> AuthService::tryRecoveryKey(std::string_view candidate) {
    // Feature off, or nothing to verify: short-circuit before touching
    // the semaphore or libsodium so a daemon with no recovery key
    // configured pays zero cost on every failed login.
    if (!cfg_.recoveryKeyHash.has_value() || candidate.empty()) {
        co_return false;
    }
    // Share login()'s memory-DoS guard: an attacker must not be able to
    // sidestep the Argon2 concurrency cap by hammering the recovery path.
    if (!loginSem_.try_acquire()) {
        co_return unexpected(makeError(ErrorCode::TooManyRequests, "login throttle"));
    }
    struct SemRelease {
        std::counting_semaphore<kMaxLoginConcurrencyCap>& s;
        ~SemRelease() { s.release(); }
    } slotGuard{loginSem_};

    co_return PasswordHasher::verify(candidate, *cfg_.recoveryKeyHash);
}

Task<Result<void>> AuthService::applyReset(std::string_view username,
                                           std::string_view newPassword) {
    if (newPassword.empty()) {
        co_return unexpected(makeError(ErrorCode::InvalidInput, "empty password"));
    }
    // The Argon2id hash below allocates ~256 MiB at the MODERATE preset,
    // same as a login verify — take a concurrency slot so a reset flood
    // can't OOM the daemon either.
    if (!loginSem_.try_acquire()) {
        co_return unexpected(makeError(ErrorCode::TooManyRequests, "login throttle"));
    }
    struct SemRelease {
        std::counting_semaphore<kMaxLoginConcurrencyCap>& s;
        ~SemRelease() { s.release(); }
    } slotGuard{loginSem_};

    auto userOpt = users_.lookupByUsername(username);
    if (!userOpt) {
        co_return unexpected(userOpt.error());
    }

    auto hashRes = PasswordHasher::hash(newPassword);
    if (!hashRes) {
        co_return unexpected(hashRes.error());
    }

    if (userOpt->has_value()) {
        // Existing user → reset the password, then invalidate every live
        // session so a previously-leaked cookie stops working (matches
        // `aid-admin reset-password`).
        if (auto set = users_.setPasswordHash((*userOpt)->id, *hashRes); !set) {
            co_return unexpected(set.error());
        }
        if (auto rv = sessions_.revokeAllFor((*userOpt)->id); !rv) {
            co_return unexpected(rv.error());
        }
        co_return Result<void>{};
    }

    // Unknown user → create it. This is the first-user bootstrap path:
    // the recovery key doubles as the only credential needed to mint the
    // very first operator account through the browser.
    if (auto created = users_.create(username, *hashRes); !created) {
        co_return unexpected(created.error());
    }
    co_return Result<void>{};
}

} // namespace aid::auth
