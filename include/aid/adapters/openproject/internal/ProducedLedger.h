#pragma once

#include <chrono>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "aid/value-types/Ids.h"

// ProducedLedger — short-TTL record of (ticketId, lockVersion) pairs this
// daemon itself just wrote to OpenProject (Phase 6).
//
// OpTicketRepo::create()/save() call record() with the lockVersion the server
// returned. When an OpenProject webhook later arrives for that same ticket,
// OpenProjectAdapter::decodeWebhook() consults contains() to tell a self-induced
// echo (our own edit bouncing back) from a genuine external edit made in the
// OpenProject UI. Matching is on the EXACT (id, version) pair — lockVersion is
// monotonic per work package, so a human edit always lands at a strictly higher
// version and is never mistaken for an echo.
//
// Entries expire after kTtl so the map stays bounded (the echo window is the
// round-trip from our PATCH to the webhook landing — well under a second with
// journal aggregation set to 0). Guarded by a mutex: in production every access
// is on the single domain loop, but the plugin-ABI contract is that
// port methods are safe to call concurrently, so we lock rather than assume.

namespace aid::adapters::openproject {

class ProducedLedger {
public:
    // Grace/retention window. Must outlive the create()/save()→webhook
    // round-trip; kept short so the map is effectively a few entries.
    static constexpr std::chrono::seconds kTtl{30};

    // The other half of the echo-window contract (lives here so both constants
    // sit together): how long decodeWebhook waits before consulting the ledger,
    // so a save()/create() racing the echo webhook has recorded its version
    // first. This is a timing cushion only — suppression is still an EXACT
    // (id, version) match, never a time window.
    static constexpr std::chrono::milliseconds kEchoGraceDelay{500};

    ProducedLedger() = default;
    ProducedLedger(const ProducedLedger&) = delete;
    ProducedLedger& operator=(const ProducedLedger&) = delete;
    ProducedLedger(ProducedLedger&&) = delete;
    ProducedLedger& operator=(ProducedLedger&&) = delete;
    ~ProducedLedger() = default;

    // Record that this daemon produced `version` of `id`. Drops expired entries
    // for the same id opportunistically so the per-id vector cannot grow without
    // bound across many edits.
    void record(const aid::TicketId& id, int version);

    // True iff a non-expired (id, version) pair was recorded — i.e. the webhook
    // at this exact version is our own echo. Expired entries are pruned on read.
    [[nodiscard]] bool contains(const aid::TicketId& id, int version);

    // One recorded production. Public only so the .cpp's TTL-prune helper can
    // name it; not part of the meaningful API surface.
    struct Entry {
        int version{0};
        std::chrono::steady_clock::time_point expiresAt{};
    };

private:
    std::mutex mtx_;
    std::unordered_map<std::string, std::vector<Entry>> byTicket_;
};

} // namespace aid::adapters::openproject
