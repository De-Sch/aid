#pragma once

#include <optional>
#include <string>
#include <vector>

#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid {

struct DashboardEntry {
    TicketId id;
    std::string subject;
    TicketStatus status = TicketStatus::New;
    StatusId statusId;
    std::vector<CallId> callIds;
    PhoneNumber callerNumber;
    std::optional<PhoneNumber> calledNumber;
    std::optional<UserHandle> assignee;
    // Call start/end as UTC instants. Serialized to the dashboard as ISO-8601
    // UTC ("...Z") and converted to the viewer's local timezone in the browser
    // (the daemon stores UTC; display localization is per-viewer).
    std::optional<Timestamp> callStart;
    std::optional<Timestamp> callEnd;
    std::string href;
    std::string projectName;
    std::optional<CallId> activeCallForViewer;
    // Other users (not the viewer) who currently hold a live call on this
    // ticket. Drives an uncolored "on call" hint in the UI; the colored
    // highlight stays exclusive to the viewer's own activeCallForViewer.
    std::vector<UserHandle> otherActiveUsers;
    // Full ticket description: auto-generated call-log lines plus the
    // freeform comments appended via /ui/comment. The UI renders this as the
    // ticket's comment/history section above the comment composer.
    std::string description;
    // Mirror of the source Ticket's optimistic-lock version + last-modified
    // instant (lockVersion bump). The live-delta protocol
    // carries lockVersion at the frame top level so a viewer can drop a stale
    // ticket_upsert/ticket_remove that races a newer one; not part of the REST
    // dashboard JSON (the frontend reads it from the WebSocket frame instead).
    int lockVersion = 0;
    Timestamp updatedAt{};
};

struct ActiveCall {
    TicketId ticketId;
    CallId callId;
    std::string projectName;
    PhoneNumber callerNumber;
};

struct DashboardView {
    std::vector<DashboardEntry> tickets;
    std::optional<ActiveCall> active;
    // Address-book hint for the viewer's active call: the Contact matched
    // by looking up the active call's caller number. nullopt when there is
    // no active call, or the address book has no match.
    std::optional<Contact> addressCallInformation;
};

} // namespace aid
