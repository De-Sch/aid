#pragma once

#include <cstdint>

#include "aid/plumbing/Error.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

// Compile-time fingerprint of the in-memory layout of every value-type that
// crosses the plugin `.so` boundary. The daemon and each plugin compute
// kPluginAbiLayoutTag from the SAME headers, so if a boundary struct is changed
// on one side but the other isn't rebuilt — exactly the mid-struct field add in
// commit 01e9995 that corrupted the heap — the two tags differ and PluginLoader
// refuses to load the skewed plugin instead of crashing.
//
// This is the layout counterpart to aid_plugin_api_version(): that versions the
// extern "C" factory CONTRACT; this versions the value-type LAYOUTS. A field
// add doesn't bump the api version, so it cannot be the guard here.
//
// Depends only on value-types/ + plumbing/ headers (no Drogon, no
// infrastructure) so any plugin — which links only aid_ports — can include it.
namespace aid::abi {

namespace detail {

// FNV-1a parameters (64-bit). FNV is a constexpr-friendly, dependency-free
// hash; we only need a stable mixing function, not cryptographic strength.
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

// Fold the 8 little-endian bytes of `value` into the running hash.
constexpr std::uint64_t mixU64(std::uint64_t hash, std::uint64_t value) noexcept {
    for (int i = 0; i < 8; ++i) {
        hash ^= (value & 0xFFULL);
        hash *= kFnvPrime;
        value >>= 8;
    }
    return hash;
}

// Fold a type's size AND alignment. Folding both catches mid-struct inserts
// (which change sizeof) and the rarer cases where a reorder shifts alignment.
template <class T> constexpr std::uint64_t foldType(std::uint64_t hash) noexcept {
    hash = mixU64(hash, sizeof(T));
    hash = mixU64(hash, alignof(T));
    return hash;
}

constexpr std::uint64_t computeLayoutTag() noexcept {
    std::uint64_t hash = kFnvOffsetBasis;
    // Id<Tag> is the same layout for every tag (a single std::string), so one
    // representative instantiation covers CallId/TicketId/UserHandle/... .
    hash = foldType<aid::CallId>(hash);
    hash = foldType<aid::PhoneNumber>(hash);
    hash = foldType<aid::Ticket>(hash);
    hash = foldType<aid::NewTicket>(hash);
    hash = foldType<aid::Contact>(hash);
    hash = foldType<aid::DashboardEntry>(hash);
    hash = foldType<aid::DashboardView>(hash);
    hash = foldType<aid::ActiveCall>(hash);
    hash = foldType<aid::WebhookDecode>(hash);
    hash = foldType<aid::plumbing::Error>(hash);
    return hash;
}

} // namespace detail

// The layout fingerprint this translation unit was compiled against.
constexpr std::uint64_t kPluginAbiLayoutTag = detail::computeLayoutTag();

} // namespace aid::abi
