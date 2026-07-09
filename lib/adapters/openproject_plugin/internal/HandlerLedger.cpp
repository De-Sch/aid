#include "aid/adapters/openproject/internal/HandlerLedger.h"

#include <algorithm>
#include <utility>

namespace aid::adapters::openproject {

void HandlerLedger::evictIfOverCapacity(std::chrono::steady_clock::time_point now) {
    if (byTicket_.size() <= kMaxTickets) {
        return;
    }
    // Drop everything already expired first — that alone usually reclaims room.
    for (auto it = byTicket_.begin(); it != byTicket_.end();) {
        if (it->second.expiresAt <= now) {
            it = byTicket_.erase(it);
        } else {
            ++it;
        }
    }
    // If churn still has us over the cap, evict the soonest-to-expire entries:
    // they are the closest to ageing out anyway and the least likely to still be
    // needed by a webhook diff.
    while (byTicket_.size() > kMaxTickets) {
        auto victim =
            std::min_element(byTicket_.begin(), byTicket_.end(), [](const auto& a, const auto& b) {
                return a.second.expiresAt < b.second.expiresAt;
            });
        if (victim == byTicket_.end()) {
            break;
        }
        byTicket_.erase(victim);
    }
}

void HandlerLedger::record(const aid::TicketId& id, const std::vector<aid::UserHandle>& handlers) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};
    auto& entry = byTicket_[id.v];
    entry.handlers = handlers;
    entry.expiresAt = now + kTtl;
    evictIfOverCapacity(now);
}

void HandlerLedger::recordIfAbsent(const aid::TicketId& id,
                                   const std::vector<aid::UserHandle>& handlers) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};
    // Defer to any live entry — a fresher write path may have recorded it while a
    // find-arm GET was in flight. An expired entry counts as absent (it would no
    // longer satisfy a webhook diff anyway), so we re-seed over it.
    if (auto it = byTicket_.find(id.v); it != byTicket_.end() && it->second.expiresAt > now) {
        return;
    }
    auto& entry = byTicket_[id.v];
    entry.handlers = handlers;
    entry.expiresAt = now + kTtl;
    evictIfOverCapacity(now);
}

std::optional<std::vector<aid::UserHandle>>
HandlerLedger::exchange(const aid::TicketId& id, const std::vector<aid::UserHandle>& handlers) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};

    std::optional<std::vector<aid::UserHandle>> prior;
    if (auto it = byTicket_.find(id.v); it != byTicket_.end() && it->second.expiresAt > now) {
        prior = it->second.handlers;
    }

    auto& entry = byTicket_[id.v];
    entry.handlers = handlers;
    entry.expiresAt = now + kTtl;
    evictIfOverCapacity(now);
    return prior;
}

} // namespace aid::adapters::openproject
