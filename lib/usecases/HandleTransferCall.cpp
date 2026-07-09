#include "aid/usecases/HandleTransferCall.h"

#include <optional>
#include <string>
#include <utility>

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

HandleTransferCall::HandleTransferCall(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui)
    : ts_(ts), ui_(ui) {
}

Task<Result<void>> HandleTransferCall::run(const aid::TransferCall& ev) {
    // Step 1: lookup by callid substring. Non-fatal if missing.
    auto found = co_await ts_.findByCallidContains(ev.callid);
    if (!found) {
        co_return aid::plumbing::unexpected{found.error()};
    }
    if (!found->has_value()) {
        co_return Result<void>{};
    }

    const aid::TicketId id = (**found).id;

    // Step 2: resolve the new user. Missing = non-fatal early return. (Done
    // before the save so the resolved login can be captured into the reducer.)
    auto resolved = co_await ts_.resolveUser(ev.newUser.v);
    if (!resolved) {
        co_return aid::plumbing::unexpected{resolved.error()};
    }
    if (!resolved->has_value()) {
        co_return Result<void>{};
    }
    const aid::UserHandle newUser = **resolved;
    const aid::CallId callid = ev.callid;

    // Step 3: persist the mutation as a pure delta applied to the fresh ticket
    // inside save() (re-applied on every 409), so a concurrent same-ticket
    // hangup/transfer is not clobbered. The CallLineFormatter
    // logic is unchanged; only WHERE it runs moved.
    //
    // Hoist the reducer to a named local to dodge gcc-12's coroutine
    // frame-lifetime bug (a temporary in the co_await operand is double-destroyed
    // — see HandleIncomingCall's NewTicket hoist).
    const aid::ports::TicketReducer reducer = [newUser, callid](aid::Ticket t) {
        // Never reopen Closed tickets.
        if (t.status != aid::TicketStatus::Closed) {
            t.status = aid::TicketStatus::InProgress;
        }

        // Set the assignee ONLY when the ticket has none. The ticket system allows a
        // single assignee; the transferred-to operator is always recorded in
        // the callHandler CSV (below) so they stay visible without churning —
        // and 422ing — an existing assignee who may not share the new handler's
        // project membership.
        if (!t.assignee.has_value()) {
            t.assignee = newUser;
        }

        // Rewrite the call-log line's username prefix to the transferred-to
        // operator (newUser, not the possibly-preserved assignee). Call-log
        // lines live in `callLength`, not `description`.
        if (const auto sp = aid::domain::CallLineFormatter::findLineFor(t.callLength, callid);
            sp.has_value()) {
            const std::string line = t.callLength.substr(sp->begin, sp->end - sp->begin);
            const std::string rewritten =
                aid::domain::CallLineFormatter::rewriteUser(line, newUser);
            t.callLength.replace(sp->begin, sp->end - sp->begin, rewritten);
        }
        return t;
    };
    auto saved = co_await ts_.save(id, reducer);
    if (!saved) {
        co_return aid::plumbing::unexpected{saved.error()};
    }

    // Step 4: record the transferred-to operator in the callHandler CSV (dedup +
    // concurrency-safe merge inside the adapter). Visibility mechanism that
    // survives non-membership; a failure propagates so the WAL can replay.
    auto recorded = co_await ts_.addCallHandler(id, newUser);
    if (!recorded) {
        co_return aid::plumbing::unexpected{recorded.error()};
    }

    // Step 5: push the live delta. Re-fetch so the entry reflects the
    // authoritative post-PATCH state — lockVersion after BOTH the save and the
    // callHandler merge, and the callHandlers CSV recipientsFor targets (so the
    // transferred-to operator reaches their own dashboard even cross-project). A
    // failed re-fetch is non-fatal: the save already succeeded.
    auto fresh = co_await ts_.fetchById(id);
    if (fresh.has_value()) {
        TicketDeltaEmitter emitter{ts_, ui_};
        (void)co_await emitter.emitTicketDelta(std::move(*fresh));
    }
    co_return Result<void>{};
}

} // namespace aid::usecases
