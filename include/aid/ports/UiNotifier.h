#pragma once

#include <string_view>

#include "aid/plumbing/ActionResult.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

// Abstract port for pushing updates to the SvelteKit UI.
// All methods fire-and-forget; the abstract port has no subscribe/unsubscribe
// — those are concrete-only concerns of WsHubAdapter.

namespace aid::ports {

class UiNotifier {
public:
    virtual ~UiNotifier() = default;

    UiNotifier() = default;
    UiNotifier(const UiNotifier&) = delete;
    UiNotifier& operator=(const UiNotifier&) = delete;
    UiNotifier(UiNotifier&&) = delete;
    UiNotifier& operator=(UiNotifier&&) = delete;

    virtual void notifyInvalidate(std::string_view scope) = 0;
    virtual void notifyInvalidateUser(UserHandle user, std::string_view scope) = 0;
    virtual void notifyActionResult(UserHandle user, const plumbing::ActionResult& result) = 0;

    // Granular live-delta push (replaces the coarse notifyInvalidate("dashboard")
    // broadcast for ticket changes). The emitter targets exactly the users in
    // TicketStore::recipientsFor, so each call is scoped to one viewer's own
    // connections. Fire-and-forget, like the other notify* methods.
    //
    //   pushTicketUpsert → {"type":"ticket_upsert","entry":{…},"lockVersion":N}
    //   pushTicketRemove → {"type":"ticket_remove","ticketId":"…","lockVersion":N}
    //
    // lockVersion lets the viewer drop a frame that lost a race with a newer one.
    virtual void pushTicketUpsert(UserHandle user, const DashboardEntry& entry) = 0;
    virtual void pushTicketRemove(UserHandle user, TicketId ticketId, int lockVersion) = 0;
};

} // namespace aid::ports
