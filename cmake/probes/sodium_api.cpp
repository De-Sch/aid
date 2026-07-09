// CMake configure-time probe: pins the exact libsodium API surface
// aid::auth::PasswordHasher uses. Linked against AID_SODIUM_LIB during
// configure; not part of the daemon.
//
// Floor: libsodium 1.0.18 (Debian bookworm). The four `crypto_pwhash_*`
// names plus `sodium_init` / `randombytes_buf` / `sodium_memzero` are
// the entire surface aid_auth touches.

#include <sodium.h>

#include <cstring>

int main() {
    if (sodium_init() < 0) {
        return 1;
    }

    char hash[crypto_pwhash_STRBYTES] = {};
    const char* pw = "probe-password";
    if (crypto_pwhash_str(hash, pw, std::strlen(pw), crypto_pwhash_OPSLIMIT_MIN,
                          crypto_pwhash_MEMLIMIT_MIN) != 0) {
        return 2;
    }

    // The MODERATE preset constants must exist as compile-time symbols
    // even if we don't invoke the slow hash with them at probe time.
    constexpr unsigned long long ops = crypto_pwhash_OPSLIMIT_MODERATE;
    constexpr size_t mem = crypto_pwhash_MEMLIMIT_MODERATE;
    (void)ops;
    (void)mem;

    if (crypto_pwhash_str_verify(hash, pw, std::strlen(pw)) != 0) {
        return 3;
    }

    // needs_rehash returns 0/1/-1; we only care that the symbol resolves.
    (void)crypto_pwhash_str_needs_rehash(hash, ops, mem);

    unsigned char token[32];
    randombytes_buf(token, sizeof(token));
    sodium_memzero(token, sizeof(token));
    return 0;
}
