#include "aid/usecases/TicketDeltaEmitter.h"

#include <string>

#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/WebhookDecode.h"

namespace aid::usecases {

using aid::plumbing::Result;
using aid::plumbing::Task;

TicketDeltaEmitter::TicketDeltaEmitter(aid::ports::TicketStore& ts,
                                       aid::ports::UiNotifier& ui) noexcept
    : ts_(ts), ui_(ui) {
}

Task<Result<void>> TicketDeltaEmitter::emitTicketDelta(aid::Ticket ticket) {
    auto recipients = co_await ts_.recipientsFor(ticket);
    if (!recipients) {
        // Every caller treats the delta as best-effort and (void)-discards this
        // error (the persisted change already succeeded). That means this is the
        // ONLY place the failure is observable — log it here so a broken
        // recipient lookup can't silently stop every live update (it once did,
        // via a 404 members endpoint). Not double-logging: nobody else logs it.
        aid::crosscutting::Logger::instance().warn(
            "TicketDeltaEmitter: recipientsFor failed for ticket " + ticket.id.v +
            ", no live delta sent: " + recipients.error().message);
        co_return aid::plumbing::unexpected{recipients.error()};
    }

    // The two dashboard arms (open call tickets + handler tickets) only ever
    // surface New / In Progress tickets, so any other status means the ticket
    // must vanish from every recipient's board rather than be upserted.
    const bool onDashboard =
        ticket.status == aid::TicketStatus::New || ticket.status == aid::TicketStatus::InProgress;

    for (const auto& viewer : *recipients) {
        if (onDashboard) {
            ui_.pushTicketUpsert(viewer, ts_.buildEntry(ticket, viewer));
        } else {
            ui_.pushTicketRemove(viewer, ticket.id, ticket.lockVersion);
        }
    }

    co_return Result<void>{};
}

Task<Result<void>> TicketDeltaEmitter::emitWebhookDelta(aid::WebhookDecode decode) {
    // 1. The usual per-recipient upsert/remove delta for the ticket's CURRENT
    //    recipients. emitTicketDelta takes its ticket by value, so passing the
    //    lvalue copies it and leaves decode.ticket intact for the removes below.
    auto emitted = co_await emitTicketDelta(decode.ticket);

    // 2. AFTER the upsert fan-out: an admin removing a callHandler (customField7)
    //    in the ticket UI drops the ticket off any viewer who saw it ONLY via
    //    that handler entry. decodeWebhook already computed those logins
    //    (handler-removed AND non-member); they are disjoint from the current
    //    recipients above, so push each a ticket_remove. Independent of
    //    recipientsFor — fire even if the upsert fan-out errored.
    for (const auto& login : decode.droppedRecipients) {
        ui_.pushTicketRemove(login, decode.ticket.id, decode.ticket.lockVersion);
    }

    co_return emitted;
}

} // namespace aid::usecases
