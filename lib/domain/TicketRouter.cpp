#include "aid/domain/TicketRouter.h"

namespace aid::domain {

TicketRouter::RoutingDecision TicketRouter::decideKnown(const KnownInput& in) {
    for (const auto& cand : in.perProject) {
        if (cand.latestOpenCallTicket.has_value()) {
            return ReuseExisting{cand.latestOpenCallTicket->id};
        }
    }
    return CreateInProject{in.perProject.front().project};
}

TicketRouter::RoutingDecision TicketRouter::decideUnknown(const UnknownInput& in) {
    if (in.byName.has_value()) {
        return ReuseExisting{in.byName->id};
    }
    if (in.byNumber.has_value()) {
        return ReuseExisting{in.byNumber->id};
    }
    return CreateInProject{in.unknownFallback};
}

} // namespace aid::domain
