#include "aid/domain/StateTransitions.h"

#include <vector>

#include "aid/value-types/Ids.h"

namespace aid::domain {

std::vector<TicketStatus> StateTransitions::path(TicketStatus from, TicketStatus to) {
    switch (from) {
    case TicketStatus::New:
        if (to == TicketStatus::InProgress) {
            return {TicketStatus::InProgress};
        }
        if (to == TicketStatus::Closed) {
            return {TicketStatus::InProgress, TicketStatus::Closed};
        }
        return {};
    case TicketStatus::InProgress:
        if (to == TicketStatus::Closed) {
            return {TicketStatus::Closed};
        }
        return {};
    case TicketStatus::Closed:
        return {};
    }
    return {};
}

bool StateTransitions::canTransitionDirect(TicketStatus from, TicketStatus to) {
    const auto p = path(from, to);
    return p.size() == 1;
}

} // namespace aid::domain
