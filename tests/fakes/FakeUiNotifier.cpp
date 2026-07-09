#include "FakeUiNotifier.h"

#include <utility>

namespace aid::fakes {

void FakeUiNotifier::notifyInvalidate(std::string_view scope) {
    try {
        invalidateScopes.emplace_back(scope);
    } catch (...) {
    }
}

void FakeUiNotifier::notifyInvalidateUser(aid::UserHandle user, std::string_view scope) {
    try {
        invalidateUserScopes.emplace_back(std::move(user), std::string{scope});
    } catch (...) {
    }
}

void FakeUiNotifier::notifyActionResult(aid::UserHandle user,
                                        const aid::plumbing::ActionResult& result) {
    try {
        actionResults.emplace_back(std::move(user), result);
    } catch (...) {
    }
}

void FakeUiNotifier::pushTicketUpsert(aid::UserHandle user, const aid::DashboardEntry& entry) {
    try {
        ticketUpserts.emplace_back(std::move(user), entry);
    } catch (...) {
    }
}

void FakeUiNotifier::pushTicketRemove(aid::UserHandle user, aid::TicketId ticketId,
                                      int lockVersion) {
    try {
        ticketRemoves.emplace_back(std::move(user), std::move(ticketId), lockVersion);
    } catch (...) {
    }
}

} // namespace aid::fakes
