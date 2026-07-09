#pragma once

#include <optional>
#include <string>
#include <vector>

#include "aid/value-types/Ids.h"

namespace aid {

struct Ticket {
    TicketId id;
    ProjectId projectId;
    std::string subject;
    TicketStatus status = TicketStatus::New;
    // Raw ticket-system status id behind `status` (e.g. "7"). Captured
    // verbatim at the parse funnel so it survives the collapse into the
    // 5-value `status` enum; surfaced to the UI as DashboardEntry.statusId.
    StatusId statusId;
    std::optional<UserHandle> assignee;
    std::vector<CallId> callIds;
    PhoneNumber callerNumber;
    std::optional<PhoneNumber> calledNumber;
    std::optional<Timestamp> callStart;
    std::optional<Timestamp> callEnd;
    std::string description;
    // Auto-generated per-callid call-log lines ("{user}: Call start: … (callid)
    // Call End: …"), appended across calls. Stored in the ticket system's
    // `callLength` custom field (a Formattable field), kept separate from
    // `description` so the comment section holds only human-typed notes.
    std::string callLength;
    // Logins of every operator who handled (accepted / made / transferred) a
    // call on this ticket. Stored in the ticket system's `callHandler` custom field
    // (customField7, a Formattable field) as a ", "-separated CSV of logins.
    // Drives dashboard visibility independent of project membership / assignee.
    std::vector<UserHandle> callHandlers;
    Timestamp updatedAt{};
    int lockVersion = 0;
};

struct NewTicket {
    ProjectId projectId;
    std::string subject;
    TicketStatus status = TicketStatus::New;
    CallId callId;
    PhoneNumber callerNumber;
    std::optional<PhoneNumber> calledNumber;
    std::optional<UserHandle> assignee;
};

} // namespace aid
