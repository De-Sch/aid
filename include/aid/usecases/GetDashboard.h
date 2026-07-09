#pragma once

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

namespace aid::ports {
class TicketStore;
class AddressBook;
} // namespace aid::ports

namespace aid::usecases {

// Orchestrates GET /ui/dashboard: composes the combined ticket list,
// the active-call hint (first entry whose activeCallForViewer is set), and the
// address-book hint for that active call into a single DashboardView. The
// adapter is responsible for the projection (href, ordering,
// activeCallForViewer per entry); this use case only stitches the view and, when
// there is an active call, looks up the caller's Contact via the AddressBook.
class GetDashboard {
public:
    GetDashboard(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::DashboardView>>
    run(aid::UserHandle viewer);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::AddressBook& ab_;
};

} // namespace aid::usecases
