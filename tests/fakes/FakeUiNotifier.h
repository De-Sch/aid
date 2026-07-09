#pragma once

#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "aid/plumbing/ActionResult.h"
#include "aid/ports/UiNotifier.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

namespace aid::fakes {

// Fire-and-forget recorder. All three notify methods are best-effort by spec,
// so allocation failures are swallowed silently.
class FakeUiNotifier final : public aid::ports::UiNotifier {
public:
    std::vector<std::string> invalidateScopes;
    std::vector<std::pair<aid::UserHandle, std::string>> invalidateUserScopes;
    std::vector<std::pair<aid::UserHandle, aid::plumbing::ActionResult>> actionResults;
    std::vector<std::pair<aid::UserHandle, aid::DashboardEntry>> ticketUpserts;
    std::vector<std::tuple<aid::UserHandle, aid::TicketId, int>> ticketRemoves;

    void notifyInvalidate(std::string_view scope) override;
    void notifyInvalidateUser(aid::UserHandle user, std::string_view scope) override;
    void notifyActionResult(aid::UserHandle user,
                            const aid::plumbing::ActionResult& result) override;
    void pushTicketUpsert(aid::UserHandle user, const aid::DashboardEntry& entry) override;
    void pushTicketRemove(aid::UserHandle user, aid::TicketId ticketId, int lockVersion) override;
};

} // namespace aid::fakes
