#include "aid/adapters/openproject/internal/ProducedLedger.h"

#include <algorithm>

namespace aid::adapters::openproject {

namespace {

// Erase entries whose expiry has passed. Caller holds the mutex.
void pruneExpired(std::vector<ProducedLedger::Entry>& entries,
                  std::chrono::steady_clock::time_point now) {
    entries.erase(
        std::remove_if(entries.begin(), entries.end(),
                       [now](const ProducedLedger::Entry& e) { return e.expiresAt <= now; }),
        entries.end());
}

} // namespace

void ProducedLedger::record(const aid::TicketId& id, int version) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};
    auto& entries = byTicket_[id.v];
    pruneExpired(entries, now);
    entries.push_back(Entry{version, now + kTtl});
}

bool ProducedLedger::contains(const aid::TicketId& id, int version) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};
    auto it = byTicket_.find(id.v);
    if (it == byTicket_.end()) {
        return false;
    }
    pruneExpired(it->second, now);
    const bool hit = std::any_of(it->second.begin(), it->second.end(),
                                 [version](const Entry& e) { return e.version == version; });
    // Reclaim the bucket once it has emptied out so the map does not retain a
    // growing set of stale ticket-id keys.
    if (it->second.empty()) {
        byTicket_.erase(it);
    }
    return hit;
}

} // namespace aid::adapters::openproject
