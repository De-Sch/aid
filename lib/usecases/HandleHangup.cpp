#include "aid/usecases/HandleHangup.h"

#include <optional>
#include <string>
#include <utility>

#include "aid/crosscutting/Clock.h"
#include "aid/domain/CallLineFormatter.h"
#include "aid/domain/CallTracker.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid::usecases {

using aid::plumbing::Result;
using aid::plumbing::Task;

HandleHangup::HandleHangup(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui,
                           aid::crosscutting::Clock& clock)
    : ts_(ts), ui_(ui), clock_(clock) {
}

Task<Result<void>> HandleHangup::run(const aid::HangupCall& ev) {
    // Step 1: lookup by callid substring. Missing ticket is a critical
    // error here — the only event that promotes "not found".
    auto found = co_await ts_.findByCallidContains(ev.callid);
    if (!found) {
        co_return aid::plumbing::unexpected{found.error()};
    }
    if (!found->has_value()) {
        co_return aid::plumbing::unexpected{
            aid::plumbing::Error{aid::plumbing::ErrorCode::InvariantViolation,
                                 "hangup for unknown callid: " + ev.callid.v, std::nullopt}};
    }

    const aid::TicketId id = (**found).id;

    // Stamp the end time once; capture it (with the callid) into the reducer so
    // every retry re-derives the same delta against fresh server state.
    const auto now = clock_.now();
    const aid::CallId callid = ev.callid;

    // The mutation is a pure delta applied to the freshly fetched ticket inside
    // save() (re-applied on every 409). It must read the ticket's CURRENT
    // callLength / callIds and edit them — never replace them from a snapshot —
    // so a concurrent same-ticket hangup/transfer is not clobbered. The
    // CallLineFormatter / CallTracker logic
    // is unchanged; only WHERE it runs moved (use-case body → reducer closure).
    //
    // Hoist the reducer to a named local: a lambda/std::function temporary left
    // in the co_await operand is double-destroyed under gcc-12's coroutine
    // frame-lifetime bug (same workaround as the NewTicket hoist in
    // HandleIncomingCall — repro under ASan as a heap corruption).
    const aid::ports::TicketReducer reducer = [now, callid](aid::Ticket t) {
        // Stamp the end time.
        t.callEnd = now;

        // Complete the call-log line for this callid (if present) by appending
        // the "Call End:" marker. Call-log lines live in `callLength`, not
        // `description`.
        if (const auto sp = aid::domain::CallLineFormatter::findLineFor(t.callLength, callid);
            sp.has_value()) {
            const std::string line = t.callLength.substr(sp->begin, sp->end - sp->begin);
            const std::string closed = aid::domain::CallLineFormatter::complete(line, now);
            t.callLength.replace(sp->begin, sp->end - sp->begin, closed);
        }

        // Remove the callid from the encoded list.
        const auto encoded = aid::domain::CallTracker::encode(t.callIds);
        const auto updated = aid::domain::CallTracker::withRemoved(encoded, callid);
        t.callIds = aid::domain::CallTracker::decode(updated);

        // Status is NOT closed automatically — agents close
        // manually via /ui/close.
        return t;
    };
    auto saved = co_await ts_.save(id, reducer);
    if (!saved) {
        co_return aid::plumbing::unexpected{saved.error()};
    }

    // Step 6: push the live delta from the post-save ticket (lockVersion bumped).
    TicketDeltaEmitter emitter{ts_, ui_};
    (void)co_await emitter.emitTicketDelta(std::move(*saved));
    co_return Result<void>{};
}

} // namespace aid::usecases
