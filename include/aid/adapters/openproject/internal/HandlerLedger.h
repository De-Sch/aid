#pragma once

#include <chrono>
#include <cstddef>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <vector>

#include "aid/value-types/Ids.h"

// HandlerLedger — short-lived record of the last-known callHandler login set the
// daemon observed for each open ticket (Phase 6 / S7). Sibling of ProducedLedger.
//
// The daemon NEVER removes a callHandler itself — a drop only ever comes from an
// admin manually editing customField7 in the OpenProject UI, delivered via the
// existing work_package:updated webhook. To turn that edit into a live
// ticket_remove for the right viewers, decodeWebhook needs to know which handler
// logins the ticket USED to carry; the webhook body only carries the new set.
// This ledger is that memory: OpTicketRepo records the freshest handler set on
// every fetch / create / save / addCallHandler, and decodeWebhook exchange()s
// the new set in while reading the prior one back out, so dropped = prior \ new.
//
// Entries are TTL'd and the map is capped so a long-lived daemon's memory stays
// bounded to roughly the currently-open ticket population (a closed ticket stops
// being fetched and ages out). Cold start: the first webhook for a ticket after
// a restart finds no prior set and surfaces nothing — it self-heals on the next
// reload / membership poll, and the REST dashboard is correct regardless because
// the dropped non-member no longer matches either visibility arm.
//
// Guarded by a mutex: in production every access is on the single domain loop,
// but the plugin-ABI contract is that port methods are safe to call
// concurrently, so we lock rather than assume.

namespace aid::adapters::openproject {

class HandlerLedger {
public:
    // How long a recorded handler set stays matchable. Generous relative to
    // ProducedLedger's echo window because an admin's handler-drop edit can land
    // long after the call's own activity last refreshed the entry — but still
    // bounded so the map cannot grow without limit across the daemon's lifetime.
    static constexpr std::chrono::hours kTtl{6};

    // Hard backstop on tracked tickets. Mirrors the order of the live-mailbox cap:
    // well above any realistic open-ticket count, present only
    // so a pathological churn cannot grow the map unbounded between TTL sweeps.
    static constexpr std::size_t kMaxTickets = 20000;

    HandlerLedger() = default;
    HandlerLedger(const HandlerLedger&) = delete;
    HandlerLedger& operator=(const HandlerLedger&) = delete;
    HandlerLedger(HandlerLedger&&) = delete;
    HandlerLedger& operator=(HandlerLedger&&) = delete;
    ~HandlerLedger() = default;

    // Overwrite the known handler set for `id`, refreshing its TTL. Called on
    // every read/write the daemon makes so the ledger tracks the freshest set it
    // has seen for each open ticket.
    void record(const aid::TicketId& id, const std::vector<aid::UserHandle>& handlers);

    // Install `handlers` for `id` ONLY when the ledger holds no live (non-expired)
    // entry for it — i.e. fill a cold/aged-out gap without ever clobbering a set a
    // write path (fetch/create/save/addCallHandler) or a prior webhook already
    // recorded. The dashboard find-arms call this to warm the baseline on a load,
    // so a handler-drop on a ticket the daemon has not otherwise touched still
    // diffs against a known prior set. The guard is what makes find-arm warming
    // race-safe: a find-arm's snapshot is a live OP read taken BEFORE its
    // suspension point, so a concurrently-resuming addCallHandler could already
    // have recorded a fresher set; recording unconditionally could overwrite that
    // fresher set with the find-arm's staler snapshot. "Only if absent" defers to
    // any existing live entry, so writes always win and find-arms merely fill gaps.
    void recordIfAbsent(const aid::TicketId& id, const std::vector<aid::UserHandle>& handlers);

    // Install `handlers` as the new known set for `id` and return the PRIOR known
    // set, or nullopt when the ticket was untracked or its entry had expired (the
    // cold-start case). The read-and-replace is atomic under the lock so a webhook
    // diffs against exactly the set the last observation recorded.
    [[nodiscard]] std::optional<std::vector<aid::UserHandle>>
    exchange(const aid::TicketId& id, const std::vector<aid::UserHandle>& handlers);

private:
    struct Entry {
        std::vector<aid::UserHandle> handlers;
        std::chrono::steady_clock::time_point expiresAt{};
    };

    // Caller holds mtx_. When the map exceeds kMaxTickets, drop expired entries
    // first, then evict soonest-to-expire entries until back at the cap. Only
    // ever runs at the cap, so the O(n) scan is rare.
    void evictIfOverCapacity(std::chrono::steady_clock::time_point now);

    std::mutex mtx_;
    std::unordered_map<std::string, Entry> byTicket_;
};

} // namespace aid::adapters::openproject
