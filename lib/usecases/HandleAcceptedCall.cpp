#include "aid/usecases/HandleAcceptedCall.h"

#include <optional>
#include <string>
#include <utility>

#include "aid/crosscutting/Clock.h"
#include "aid/domain/CallLineFormatter.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid::usecases {

using aid::plumbing::Result;
using aid::plumbing::Task;

HandleAcceptedCall::HandleAcceptedCall(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui,
                                       aid::crosscutting::Clock& clock)
    : ts_(ts), ui_(ui), clock_(clock) {
}

Task<Result<void>> HandleAcceptedCall::run(const aid::AcceptedCall& ev) {
    // Step 1: lookup by callid "contains" — the callid field may hold
    // several comma-joined callids (a multi-call ticket), so an exact match
    // would miss the second-and-later accepts. Missing ticket is
    // non-fatal: the phone system can deliver Accept before Ring, or Ring may have been
    // dropped.
    auto found = co_await ts_.findByCallidContains(ev.callid);
    if (!found) {
        co_return aid::plumbing::unexpected{found.error()};
    }
    if (!found->has_value()) {
        co_return Result<void>{};
    }

    const aid::TicketId id = (**found).id;

    // Step 2: if event has a user, resolve them. Missing user (nullopt) is
    // non-fatal: skip the save and return. The resolved login is recorded as a
    // call handler (step 5) regardless of whether it becomes the assignee.
    std::optional<aid::UserHandle> handler;
    if (ev.user.has_value()) {
        auto resolved = co_await ts_.resolveUser(ev.user->v);
        if (!resolved) {
            co_return aid::plumbing::unexpected{resolved.error()};
        }
        if (!resolved->has_value()) {
            co_return Result<void>{};
        }
        handler = **resolved;
    }

    // Step 3: stamp THIS accept's start time. Updated on every accept (no
    // longer set-once): callStartTimestamp reflects the most recent call's
    // start, while the per-call history is preserved in the callLength
    // breadcrumb lines (each line keeps the time it was written with).
    const auto now = clock_.now();
    const aid::CallId callid = ev.callid;

    // Step 4: persist the mutation as a pure delta applied to the fresh ticket
    // inside save() (re-applied on every 409), so a concurrent same-ticket
    // accept/hangup is not clobbered. The CallLineFormatter
    // logic is unchanged; only WHERE it runs moved.
    //
    // Hoist the reducer to a named local to dodge gcc-12's coroutine
    // frame-lifetime bug (a temporary in the co_await operand is double-destroyed
    // — see HandleIncomingCall's NewTicket hoist).
    const aid::ports::TicketReducer reducer = [handler, now, callid](aid::Ticket t) {
        // Set the assignee ONLY when the ticket has none. The ticket system allows a
        // single assignee; the first handler keeps it, later handlers stay
        // visible through the callHandler CSV instead of churning — and 422ing —
        // the assignee when the new handler is not a member of the project.
        if (handler.has_value() && !t.assignee.has_value()) {
            t.assignee = *handler;
        }

        // Never reopen Closed tickets.
        if (t.status != aid::TicketStatus::Closed) {
            t.status = aid::TicketStatus::InProgress;
        }

        t.callStart = now;

        // Append the open call-start line if THIS event resolved a user and no
        // prior line for this exact (callid) exists — dedup against retries.
        // Gates on `handler` (the user this accept actually resolved), not
        // `t.assignee` (the ticket's sticky, first-handler-wins assignee) —
        // those diverge once a second user accepts a ticket someone else
        // already holds, and the line must say who is on THIS call, not who
        // holds the assignee slot. If the phone system omitted `user` on this event, no
        // line is appended — never guess from a possibly-stale assignee. The
        // call-log lines live in the `callLength` field (not `description`,
        // which holds only human comments).
        if (handler.has_value()) {
            const auto& user = *handler;
            if (!aid::domain::CallLineFormatter::hasLine(t.callLength, user, callid)) {
                if (!t.callLength.empty()) {
                    t.callLength += "\n";
                }
                t.callLength +=
                    aid::domain::CallLineFormatter::buildStart(user, *t.callStart, callid);
            }
        }
        return t;
    };
    auto saved = co_await ts_.save(id, reducer);
    if (!saved) {
        co_return aid::plumbing::unexpected{saved.error()};
    }

    // Step 5: record the accepting handler in the callHandler CSV (dedup +
    // concurrency-safe merge inside the adapter). This is the visibility
    // mechanism that survives even when the handler is not the assignee /
    // not a project member, so a failure must propagate (the WAL keeps the
    // event for replay) rather than be swallowed.
    if (handler.has_value()) {
        auto recorded = co_await ts_.addCallHandler(id, *handler);
        if (!recorded) {
            co_return aid::plumbing::unexpected{recorded.error()};
        }
    }

    // Step 6: push the live delta. Re-fetch so the entry reflects the
    // authoritative post-PATCH state — the lockVersion after BOTH the save and
    // the callHandler merge, and the callHandlers CSV that recipientsFor targets
    // (so a cross-project accepting operator reaches their own dashboard). A
    // failed re-fetch is non-fatal: the save already succeeded.
    auto fresh = co_await ts_.fetchById(id);
    if (fresh.has_value()) {
        TicketDeltaEmitter emitter{ts_, ui_};
        (void)co_await emitter.emitTicketDelta(std::move(*fresh));
    }
    co_return Result<void>{};
}

} // namespace aid::usecases
