#include "aid/auth/PasswordHasher.h"

#include <sodium.h>

#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "aid/plumbing/Error.h"

namespace aid::auth {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

namespace {

// The fixed dummy plaintext used by the timing-equal login path. It is
// deliberately not a guessable password — exposure of the hash leaks
// nothing exploitable (verifying any user-supplied password against it
// would fail).
constexpr const char* kDummyPlaintext = "aid-daemon-dummy-password-v1";

// REASON: the MODERATE preset costs ~256 MiB RAM per hash, which starves a
// small VPS/appliance where AID is meant to be installable. 32 MiB / ops=3 is
// well above libsodium's argon2id minimums and stays strong for a trusted-LAN
// internal login. The only downside — marginally easier *offline* cracking if
// auth.db is stolen — is a non-issue under our trust model. Both call sites
// (hash + needsRehash) MUST use these same values or every stored hash is
// falsely flagged for rehash. verify() reads params from the stored string, so
// pre-existing MODERATE hashes keep verifying and self-rehash on next login.
constexpr unsigned long long kArgonOps = 3ULL;         // keep ops >= 3
constexpr std::size_t kArgonMem = 32ULL * 1024 * 1024; // 32 MiB

[[nodiscard]] Error makeError(ErrorCode code, std::string msg) {
    return Error{code, std::move(msg), std::nullopt};
}

// Owning, zero-on-drop byte buffer. Plaintext passwords flow through
// this so they never linger in heap memory longer than the call.
class ScrubbedBuf {
public:
    explicit ScrubbedBuf(std::string_view src) : data_(src.size()) {
        if (!src.empty()) {
            std::memcpy(data_.data(), src.data(), src.size());
        }
    }
    ScrubbedBuf(const ScrubbedBuf&) = delete;
    ScrubbedBuf& operator=(const ScrubbedBuf&) = delete;
    ScrubbedBuf(ScrubbedBuf&&) = delete;
    ScrubbedBuf& operator=(ScrubbedBuf&&) = delete;
    ~ScrubbedBuf() {
        if (!data_.empty()) {
            sodium_memzero(data_.data(), data_.size());
        }
    }
    [[nodiscard]] const char* data() const noexcept {
        return reinterpret_cast<const char*>(data_.data());
    }
    [[nodiscard]] std::size_t size() const noexcept { return data_.size(); }

private:
    std::vector<unsigned char> data_;
};

} // namespace

void PasswordHasher::initialize() {
    static std::once_flag flag;
    static int rc = 0;
    std::call_once(flag, [] { rc = sodium_init(); });
    if (rc < 0) {
        std::abort();
    }
}

Result<std::string> PasswordHasher::hash(std::string_view plaintext) {
    if (plaintext.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "empty password"));
    }
    initialize();

    ScrubbedBuf pw{plaintext};

    std::string out;
    out.resize(crypto_pwhash_STRBYTES);
    if (crypto_pwhash_str(out.data(), pw.data(), pw.size(), kArgonOps, kArgonMem) != 0) {
        // libsodium documents this as out-of-memory. The caller treats it as
        // transient.
        return unexpected(
            makeError(ErrorCode::Unknown, "crypto_pwhash_str failed (out of memory?)"));
    }
    // crypto_pwhash_str writes a C-string; trim the trailing zero-padded tail.
    out.resize(std::strlen(out.c_str()));
    return out;
}

bool PasswordHasher::verify(std::string_view plaintext, std::string_view storedHash) {
    if (storedHash.empty()) {
        return false;
    }
    initialize();

    ScrubbedBuf pw{plaintext};

    // crypto_pwhash_str_verify wants a NUL-terminated hash string.
    // string_view doesn't guarantee that, so copy.
    std::string h{storedHash};
    if (h.size() >= crypto_pwhash_STRBYTES) {
        return false;
    }
    return crypto_pwhash_str_verify(h.c_str(), pw.data(), pw.size()) == 0;
}

bool PasswordHasher::needsRehash(std::string_view storedHash) {
    if (storedHash.empty()) {
        return true;
    }
    initialize();
    std::string h{storedHash};
    if (h.size() >= crypto_pwhash_STRBYTES) {
        return true;
    }
    // Returns 0 (no), 1 (yes), or -1 (cannot parse). Treat -1 as yes
    // so a corrupted hash gets re-issued on next successful login.
    return crypto_pwhash_str_needs_rehash(h.c_str(), kArgonOps, kArgonMem) != 0;
}

const std::string& PasswordHasher::dummyHash() {
    static std::once_flag flag;
    static std::string cached;
    std::call_once(flag, [] {
        auto r = hash(kDummyPlaintext);
        if (!r) {
            // initialize() guarantees libsodium is up; only OOM gets us
            // here, and we have no useful recovery from that during a
            // one-time precompute on the startup path.
            std::abort();
        }
        cached = std::move(*r);
    });
    return cached;
}

} // namespace aid::auth
