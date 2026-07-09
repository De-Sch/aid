#pragma once

#include <vector>

#include "aid/value-types/Ids.h"

namespace aid::domain {

// Owns the ticket system's workflow-transition graph. Pure: no I/O.
//
// The graph table:
//   New        -> InProgress : direct, path = [InProgress]
//   New        -> Closed     : two-step, path = [InProgress, Closed]
//                              (the ticket system forbids direct New->Closed)
//   InProgress -> Closed     : direct, path = [Closed]
//   Closed     -> anything   : path = [] (terminal)
//   anything else            : path = [], direct = false
//
// The *walk* over the path (PATCH + 409 retry + lockVersion refresh)
// lives in adapters/OpTicketRepo::closeTwoStep. This class owns only the graph.
class StateTransitions {
public:
    [[nodiscard]] static bool canTransitionDirect(TicketStatus from, TicketStatus to);
    [[nodiscard]] static std::vector<TicketStatus> path(TicketStatus from, TicketStatus to);
};

} // namespace aid::domain
