#pragma once

#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/MembershipDelta.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::usecases {

// Phase-1 membership reconciler (core side of the poll loop). The ticket system
// emits no membership webhook, so the plugin polls and diffs each project's member set
// into MembershipDelta{project, added, removed}; this use case turns those
// deltas into precise per-recipient live frames, exactly mirroring
// TicketDeltaEmitter's recipientsFor → buildEntry → pushTicket* shape:
//
//   for each delta:
//     for each open call ticket in delta.project:
//       added login   → ui.pushTicketUpsert(login, ts.buildEntry(ticket, login))
//       removed login  → IFF login is not a recorded callHandler of the ticket
//                       → ui.pushTicketRemove(login, ticket.id, ticket.lockVersion)
//
// A removed member who is still a callHandler keeps visibility through the
// cross-project handler arm, so they are deliberately NOT removed.
//
// The heavy openCallsInProject query is run ONLY for a project whose membership
// actually changed (the caller — the timer — already gated refreshMembership on
// ≥1 connected dashboard). Best-effort throughout: a per-project ports error is
// logged and skipped, never fatal, so one unreadable project can't stall the
// rest. Plugin-side logs do not reach backend.log, so every applied change is
// logged HERE — this use case is the only observability point for the poll loop.
//
// Depends only on the two ports, so it stays in the use-case layer with no
// Drogon / JSON / adapter coupling. Construct it from the refs the timer holds.
class ReconcileMemberships {
public:
    ReconcileMemberships(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui) noexcept;

    ReconcileMemberships(const ReconcileMemberships&) = delete;
    ReconcileMemberships& operator=(const ReconcileMemberships&) = delete;
    ReconcileMemberships(ReconcileMemberships&&) = delete;
    ReconcileMemberships& operator=(ReconcileMemberships&&) = delete;
    ~ReconcileMemberships() = default;

    // Apply a batch of membership deltas. Taken by value so the coroutine frame
    // owns the deltas across the per-project openCallsInProject co_await. Always
    // reports success: individual project failures are logged, not propagated,
    // because the poll loop is eventually consistent and must keep running.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    reconcile(std::vector<aid::MembershipDelta> deltas);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
};

} // namespace aid::usecases
