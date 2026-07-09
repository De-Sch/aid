#pragma once

#include <optional>
#include <span>
#include <variant>

#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid::domain {

// Pure routing policy: given the use case's already-fetched candidates,
// decide whether to reuse an existing open ticket or create a new one.
// Pure: no I/O — callers (use cases) own the TicketStore round trips.
//
// By contract, candidates passed in MUST
// already exclude Closed tickets ("never reopen a closed ticket"). The
// search-filter is the TicketStore's job, not this class's.
class TicketRouter {
public:
    struct ReuseExisting {
        TicketId ticket;
    };
    struct CreateInProject {
        ProjectId project;
    };
    using RoutingDecision = std::variant<ReuseExisting, CreateInProject>;

    struct RoutingCandidate {
        ProjectId project;
        std::optional<aid::Ticket> latestOpenCallTicket;
    };

    // Precondition: `perProject` is non-empty. Caller iterates over
    // `contact.projectIds[i]` so this holds by construction.
    struct KnownInput {
        std::span<const RoutingCandidate> perProject;
    };

    struct UnknownInput {
        std::optional<aid::Ticket> byName;
        std::optional<aid::Ticket> byNumber;
        ProjectId unknownFallback;
    };

    [[nodiscard]] static RoutingDecision decideKnown(const KnownInput& in);
    [[nodiscard]] static RoutingDecision decideUnknown(const UnknownInput& in);
};

} // namespace aid::domain
