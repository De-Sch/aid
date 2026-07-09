#pragma once

#include <string>

#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ids.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::usecases {

// Orchestrates POST /ui/comment/{ticketId}: trims the text, fetches
// the ticket, appends the comment as a newline-separated entry on the
// description, saves, broadcasts an ActionResult. Expected failures (empty
// text, ticket not found) ride inside ActionResult{ok=false}; only
// unexpected errors propagate via the outer Result.
class AppendComment {
public:
    AppendComment(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::plumbing::ActionResult>>
    run(aid::TicketId id, std::string text, aid::UserHandle viewer);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
};

} // namespace aid::usecases
