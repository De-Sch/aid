#include "aid/usecases/CloseTicket.h"

#include <optional>
#include <string>
#include <utility>

#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/TicketDeltaEmitter.h"

namespace aid::usecases {

using aid::plumbing::ActionResult;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;

CloseTicket::CloseTicket(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui)
    : ts_(ts), ui_(ui) {
}

Task<Result<ActionResult>> CloseTicket::run(aid::TicketId id, aid::UserHandle viewer) {
    auto r = co_await ts_.close(id);

    ActionResult ar;
    ar.op = "TICKET_CLOSE";
    ar.ticketId = id;
    if (r.has_value()) {
        ar.ok = true;
        ar.message = std::nullopt;
    } else if (r.error().code == ErrorCode::NotFound) {
        ar.ok = false;
        ar.message = "ticket not found";
    } else {
        ar.ok = false;
        ar.message = std::string{"close failed: "} + r.error().message;
    }

    ui_.notifyActionResult(viewer, ar);
    // On a successful close the ticket leaves every dashboard — re-fetch the
    // now-Closed ticket and let the emitter turn it into a ticket_remove for
    // each recipient. Nothing to push if the close itself failed (the board
    // didn't change); a failed re-fetch is likewise non-fatal.
    if (r.has_value()) {
        auto fresh = co_await ts_.fetchById(id);
        if (fresh.has_value()) {
            TicketDeltaEmitter emitter{ts_, ui_};
            (void)co_await emitter.emitTicketDelta(std::move(*fresh));
        }
    }
    co_return ar;
}

} // namespace aid::usecases
