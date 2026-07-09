#pragma once

#include <optional>
#include <string>

#include "aid/value-types/Ids.h"

namespace aid::plumbing {

struct ActionResult {
    bool ok{false};
    std::string op; // "COMMENT_SAVE" | "TICKET_CLOSE"
    aid::TicketId ticketId;
    std::optional<std::string> message;
};

} // namespace aid::plumbing
