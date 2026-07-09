#pragma once

#include <vector>

#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

// WebhookDecode — the result of decoding a genuine (non-echo) ticket-system
// webhook (Phase 6 / S7). It pairs the Ticket the webhook describes with the
// set of recipients who LOST visibility of that ticket as a side effect of the
// edit — specifically the logins an admin removed from the callHandler CSV
// (customField7) who are not project members and therefore no longer belong on
// the ticket's dashboards.
//
// `droppedRecipients` is empty for the common edit (status / subject / field
// change with no handler removed) and for the cold-start case (first webhook
// for a ticket after a restart — no prior handler set to diff against). The
// webhook consumer pushes a ticket_remove to each dropped recipient and the
// usual per-recipient ticket_upsert to the ticket's current recipients.
//
// Crosses the plugin `.so` boundary (returned from TicketStore::decodeWebhook),
// so it lives in value-types and is folded into the ABI layout tag.

namespace aid {

struct WebhookDecode {
    Ticket ticket;
    std::vector<UserHandle> droppedRecipients;
};

} // namespace aid
