#pragma once

#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::usecases {

// Orchestrates POST /ui/close/{ticketId}: walks a ticket to Closed via
// the ticket system's status path and broadcasts an ActionResult. The two-step
// transition + 409 retry lives entirely in the adapter — this use case sees
// one TicketStore::close(id) call. Pure orchestration: no JSON, no Drogon.
class CloseTicket {
public:
    CloseTicket(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::plumbing::ActionResult>>
    run(aid::TicketId id, aid::UserHandle viewer);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
};

} // namespace aid::usecases
