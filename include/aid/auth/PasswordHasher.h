#pragma once

#include <string>
#include <string_view>

#include "aid/plumbing/Result.h"

namespace aid::auth {

// PasswordHasher wraps libsodium's Argon2id primitives at the
// "moderate" preset. All methods are static; the class is uninstantiable.
//
// Bootstrap: Main() (slice 1: tests) must call initialize() once
// before any other method on this class. Subsequent calls are cheap
// no-ops; sodium_init's documented contract is that it's safe to call
// concurrently and idempotently.
class PasswordHasher {
public:
    PasswordHasher() = delete;

    // Idempotent. Calls sodium_init under a std::once_flag and aborts
    // the process if libsodium returns a fatal status — per spec there
    // is no recovery from this.
    static void initialize();

    // crypto_pwhash_str at OPSLIMIT_MODERATE / MEMLIMIT_MODERATE.
    // Returns the ~100-byte encoded string. Empty plaintext returns
    // Error{InvalidInput, "empty password"}.
    [[nodiscard]] static aid::plumbing::Result<std::string> hash(std::string_view plaintext);

    // crypto_pwhash_str_verify; constant-time. Returns false (no
    // exception) for empty or malformed storedHash. The plaintext side
    // is held in a local copy that is sodium_memzero'd on return.
    [[nodiscard]] static bool verify(std::string_view plaintext, std::string_view storedHash);

    // True iff storedHash's encoded params are weaker than the current
    // OPSLIMIT/MEMLIMIT — used by the login flow to upgrade hashes on
    // a successful verify. Treats unparseable hashes as needing rehash.
    [[nodiscard]] static bool needsRehash(std::string_view storedHash);

    // Precomputed Argon2id hash of a fixed, public dummy string.
    // Lazily computed on first call and cached for the lifetime of the
    // process. The login flow verify()s against this when the username
    // lookup misses, so the unknown-username path costs the same wall
    // clock as the legitimate path.
    [[nodiscard]] static const std::string& dummyHash();
};

} // namespace aid::auth
