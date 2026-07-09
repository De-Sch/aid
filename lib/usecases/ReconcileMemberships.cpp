#include "aid/usecases/ReconcileMemberships.h"

#include <algorithm>
#include <string>

#include "aid/crosscutting/Logger.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid::usecases {

using aid::plumbing::Result;
using aid::plumbing::Task;

namespace {

// ", "-joined login list for the human-readable change log (empty → "").
[[nodiscard]] std::string joinLogins(const std::vector<aid::UserHandle>& logins) {
    std::string out;
    for (const auto& login : logins) {
        if (!out.empty()) {
            out += ", ";
        }
        out += login.v;
    }
    return out;
}

} // namespace

ReconcileMemberships::ReconcileMemberships(aid::ports::TicketStore& ts,
                                           aid::ports::UiNotifier& ui) noexcept
    : ts_(ts), ui_(ui) {
}

Task<Result<void>> ReconcileMemberships::reconcile(std::vector<aid::MembershipDelta> deltas) {
    for (const auto& delta : deltas) {
        auto openCalls = co_await ts_.openCallsInProject(delta.project);
        if (!openCalls) {
            // Best-effort: the heavy listing failed for this one project. Log and
            // move on so an unreadable project can't stall reconciliation of the
            // others. This is the ONLY place the failure is observable (the
            // plugin's own logs never reach backend.log).
            aid::crosscutting::Logger::instance().warn(
                "ReconcileMemberships: openCallsInProject failed for project " + delta.project.v +
                ", skipping: " + openCalls.error().message);
            continue;
        }

        int frames = 0;
        for (const auto& ticket : *openCalls) {
            // Newly-added members gain the ticket on their board immediately.
            for (const auto& login : delta.added) {
                ui_.pushTicketUpsert(login, ts_.buildEntry(ticket, login));
                ++frames;
            }
            // Removed members lose it — UNLESS they are still a recorded
            // callHandler, in which case the cross-project handler arm keeps
            // them visible and a remove would wrongly hide their own call.
            for (const auto& login : delta.removed) {
                const bool stillHandler =
                    std::find(ticket.callHandlers.begin(), ticket.callHandlers.end(), login) !=
                    ticket.callHandlers.end();
                if (!stillHandler) {
                    ui_.pushTicketRemove(login, ticket.id, ticket.lockVersion);
                    ++frames;
                }
            }
        }

        // Plugin-side logs don't reach backend.log, so the poll loop's only
        // observability lives here: one line per changed project.
        aid::crosscutting::Logger::instance().info(
            "membership \xCE\x94 project=" + delta.project.v + " +[" + joinLogins(delta.added) +
            "] -[" + joinLogins(delta.removed) + "], frames=" + std::to_string(frames));
    }

    co_return Result<void>{};
}

} // namespace aid::usecases
