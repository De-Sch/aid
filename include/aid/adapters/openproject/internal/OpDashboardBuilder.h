#pragma once

#include <string>
#include <vector>

#include "aid/adapters/openproject/internal/OpTicketRepo.h"
#include "aid/adapters/openproject/internal/OpUserRepo.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

// OpDashboardBuilder — implement the 8-step pipeline as a single
// focused class. The dashboard contract:
//
//   1. projects-for-user
//   2. open call tickets in those projects (status in {New, InProgress})
//   3. tickets assigned to viewer (any type, excluding Closed)
//   4. merge + dedup by ticket id
//   5. filter — keep only tickets whose project is in the viewer's list
//   6. project each ticket to a DashboardEntry, set href + activeCallForViewer
//   7. sort: New first, then InProgress, then updatedAt desc
//   8. return entries
//
// This class is the only owner of projectWebBaseUrl — keeping URL
// construction in exactly one place.

namespace aid::adapters::openproject {

class OpDashboardBuilder {
public:
    OpDashboardBuilder(OpUserRepo& users, OpTicketRepo& tickets,
                       const aid::crosscutting::TicketSystemConfig& opCfg,
                       const aid::crosscutting::UiConfig& uiCfg);

    OpDashboardBuilder(const OpDashboardBuilder&) = delete;
    OpDashboardBuilder& operator=(const OpDashboardBuilder&) = delete;
    OpDashboardBuilder(OpDashboardBuilder&&) = delete;
    OpDashboardBuilder& operator=(OpDashboardBuilder&&) = delete;
    ~OpDashboardBuilder() = default;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::DashboardEntry>>>
    build(aid::UserHandle viewer);

    // Step-6 per-ticket projection, factored out of build() so the live-delta
    // path (TicketStore::buildEntry → TicketDeltaEmitter) produces a
    // DashboardEntry byte-identical to the one the dashboard list returns:
    // same href, same activeCallForViewer / otherActiveUsers, same statusId.
    // Pure (no I/O): only the configured projectNames map + projectWebBaseUrl.
    [[nodiscard]] aid::DashboardEntry buildEntry(const aid::Ticket& t,
                                                 aid::UserHandle viewer) const;

    // Exposed for unit-tests of the merge-dedup step. The pipeline is
    // intentionally hard to test piecewise (each step depends on the
    // previous), so the assertion surface is "given two halves and a
    // viewer-project filter, build() emits the right entries."
    [[nodiscard]] static std::vector<aid::Ticket> mergeById(std::vector<aid::Ticket> callTickets,
                                                            std::vector<aid::Ticket> assigned);

    // ProjectId → human name from the configured projectNames map.
    // Returns the projectId.v unchanged if the operator hasn't surfaced
    // a name in config — the dashboard href is still well-formed, just
    // numeric.
    [[nodiscard]] std::string projectName(const aid::ProjectId& id) const;

private:
    OpUserRepo& users_;
    OpTicketRepo& tickets_;
    const aid::crosscutting::TicketSystemConfig& opCfg_;
    const aid::crosscutting::UiConfig& uiCfg_;
};

} // namespace aid::adapters::openproject
