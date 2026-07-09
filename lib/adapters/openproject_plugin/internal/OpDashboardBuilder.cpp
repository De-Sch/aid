#include "aid/adapters/openproject/internal/OpDashboardBuilder.h"

#include <algorithm>
#include <string>
#include <unordered_set>
#include <utility>

#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/domain/CallLineFormatter.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

namespace aid::adapters::openproject {

OpDashboardBuilder::OpDashboardBuilder(OpUserRepo& users, OpTicketRepo& tickets,
                                       const aid::crosscutting::TicketSystemConfig& opCfg,
                                       const aid::crosscutting::UiConfig& uiCfg)
    : users_(users), tickets_(tickets), opCfg_(opCfg), uiCfg_(uiCfg) {
}

std::vector<aid::Ticket> OpDashboardBuilder::mergeById(std::vector<aid::Ticket> callTickets,
                                                       std::vector<aid::Ticket> assigned) {
    std::unordered_set<std::string> seen;
    seen.reserve(callTickets.size() + assigned.size());

    std::vector<aid::Ticket> out;
    out.reserve(callTickets.size() + assigned.size());

    for (auto& t : callTickets) {
        if (seen.insert(t.id.v).second) {
            out.push_back(std::move(t));
        }
    }
    for (auto& t : assigned) {
        if (seen.insert(t.id.v).second) {
            out.push_back(std::move(t));
        }
    }
    return out;
}

std::string OpDashboardBuilder::projectName(const aid::ProjectId& id) const {
    auto it = opCfg_.projectNames.find(id);
    if (it != opCfg_.projectNames.end()) {
        return it->second;
    }
    // Defensive default — the dashboard link still works, just shows
    // the numeric id rather than the human label. Logging would belong
    // in Main at config-load time, not here in the hot path.
    return id.v;
}

Task<Result<std::vector<aid::DashboardEntry>>> OpDashboardBuilder::build(aid::UserHandle viewer) {
    // Step 1 — projects-for-viewer.
    auto projects = co_await users_.projectsForUser(viewer);
    if (!projects)
        co_return unexpected(projects.error());

    // Step 2 — open call tickets in those projects.
    auto callTickets = co_await tickets_.findCallTicketsInProjectsOpen(*projects);
    if (!callTickets)
        co_return unexpected(callTickets.error());

    // Step 3 — open call tickets where the viewer is a recorded call handler.
    // This is the cross-project visibility arm: a user who handled a call in a
    // project they are NOT a member of must still see that ticket. It replaces
    // the old assignee arm (which was then re-filtered down to member projects,
    // hiding exactly those cross-project tickets).
    auto handlerTickets = co_await tickets_.findCallTicketsWithHandler(viewer);
    if (!handlerTickets)
        co_return unexpected(handlerTickets.error());

    // Step 4 — merge + dedup. No project-membership post-filter: arm A is
    // already scoped to the viewer's projects by its query, and arm B is
    // INTENTIONALLY cross-project. The union therefore matches recipientsFor
    // exactly (members ∪ callHandlers), so no ghost rows and no missing rows.
    auto merged = mergeById(std::move(*callTickets), std::move(*handlerTickets));

    // Step 5 (done before the step-6 projection) — sort on the Ticket side so we still
    // have updatedAt available. Status rank first (New < InProgress <
    // Closed); ties broken by updatedAt descending,
    // then ticket-id ascending for determinism.
    auto statusRank = [](aid::TicketStatus s) {
        switch (s) {
        case aid::TicketStatus::New:
            return 0;
        case aid::TicketStatus::InProgress:
            return 1;
        case aid::TicketStatus::Closed:
            return 2;
        }
        return 99;
    };
    std::sort(merged.begin(), merged.end(),
              [&statusRank](const aid::Ticket& a, const aid::Ticket& b) {
                  const int ra = statusRank(a.status);
                  const int rb = statusRank(b.status);
                  if (ra != rb)
                      return ra < rb;
                  if (a.updatedAt != b.updatedAt)
                      return a.updatedAt > b.updatedAt; // desc
                  return a.id.v < b.id.v;
              });

    // Step 6 — project each surviving ticket to a DashboardEntry, compute
    // href + activeCallForViewer. Delegated to buildEntry so the single-ticket
    // live-delta path stays byte-identical to the list path.
    std::vector<aid::DashboardEntry> entries;
    entries.reserve(merged.size());
    for (const auto& t : merged) {
        entries.push_back(buildEntry(t, viewer));
    }

    co_return entries;
}

aid::DashboardEntry OpDashboardBuilder::buildEntry(const aid::Ticket& t,
                                                   aid::UserHandle viewer) const {
    aid::DashboardEntry e;
    e.id = t.id;
    e.subject = t.subject;
    e.status = t.status;
    e.statusId = t.statusId;
    e.callIds = t.callIds;
    e.callerNumber = t.callerNumber;
    e.calledNumber = t.calledNumber;
    e.assignee = t.assignee;
    e.callStart = t.callStart;
    e.callEnd = t.callEnd;

    std::string proj = projectName(t.projectId);

    const std::string& webBase = uiCfg_.projectWebBaseUrl;
    std::string href;
    href.reserve(webBase.size() + 1 + proj.size() + 32);
    href.append(webBase);
    if (!webBase.empty() && webBase.back() != '/')
        href.push_back('/');
    href.append(proj);
    href.append("/work_packages/");
    href.append(e.id.v);
    e.href = std::move(href);
    e.projectName = std::move(proj);

    // Active-call detection scans the call-log lines, which now live in the
    // `callLength` field (not `description`, which holds only human comments).
    e.activeCallForViewer =
        aid::domain::CallLineFormatter::findOpenLineForUser(t.callLength, viewer);
    // Other users with a live call on this ticket — surfaced uncolored in
    // the UI. Exclude the viewer here so the hint never duplicates the
    // row's own activeCallForViewer ("Live") state.
    for (auto& u : aid::domain::CallLineFormatter::findUsersWithOpenCalls(t.callLength)) {
        if (u.v != viewer.v) {
            e.otherActiveUsers.push_back(std::move(u));
        }
    }
    e.description = t.description;
    e.lockVersion = t.lockVersion;
    e.updatedAt = t.updatedAt;
    return e;
}

} // namespace aid::adapters::openproject
