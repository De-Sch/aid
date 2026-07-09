#include "aid/serialization/DashboardJson.h"

#include <chrono>
#include <ctime>
#include <iomanip>
#include <sstream>
#include <string>

#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/TimeFormat.h"

namespace aid::serialization {

namespace {

[[nodiscard]] const char* toString(aid::TicketStatus s) noexcept {
    switch (s) {
    case aid::TicketStatus::New:
        return "New";
    case aid::TicketStatus::InProgress:
        return "InProgress";
    case aid::TicketStatus::Closed:
        return "Closed";
    }
    return "Unknown";
}

// Serialize an instant as the daemon's LOCAL wall-clock "YYYY-MM-DD HH:MM:SS"
// — the same basis as the stored callStart/callEnd custom fields and the
// callLength breadcrumb, so the dashboard shows exactly what is in the ticket system.
// The machine's TZ (set per-deployment via the environment) governs; no zone
// is hardcoded.
[[nodiscard]] std::string formatLocalTimestamp(aid::Timestamp t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    ::localtime_r(&tt, &tm);
    std::ostringstream os;
    os << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return os.str();
}

// Serialize an instant as ISO-8601 UTC "YYYY-MM-DDTHH:MM:SSZ". Unlike the
// local wall-clock callStart/callEnd above, updatedAt is a machine/audit field
// the frontend never displays — it carries it only to re-sort live deltas by
// the same "status rank → updatedAt desc → id" order the server uses. A
// fixed-width UTC string is monotonic w.r.t. the underlying Timestamp, so the
// client's lexicographic compare matches the server's Timestamp compare (and,
// being UTC, has no DST-fold ambiguity). Shares aid::formatIso8601Utc with
// aid-admin; the local callStart/callEnd above stay on their own localtime_r
// helper on purpose.
[[nodiscard]] std::string formatUtcTimestamp(aid::Timestamp t) {
    return aid::formatIso8601Utc(t);
}

} // namespace

nlohmann::json toJson(const aid::DashboardEntry& e) {
    nlohmann::json j;
    j["id"] = e.id.v;
    j["subject"] = e.subject;
    j["status"] = toString(e.status);
    j["statusId"] = e.statusId.v;
    auto cids = nlohmann::json::array();
    for (const auto& c : e.callIds) {
        cids.push_back(c.v);
    }
    j["callIds"] = std::move(cids);
    j["callerNumber"] = e.callerNumber.v;
    if (e.calledNumber.has_value()) {
        j["calledNumber"] = e.calledNumber->v;
    } else {
        j["calledNumber"] = nullptr;
    }
    if (e.assignee.has_value()) {
        j["assignee"] = e.assignee->v;
    } else {
        j["assignee"] = nullptr;
    }
    if (e.callStart.has_value()) {
        j["callStart"] = formatLocalTimestamp(*e.callStart);
    } else {
        j["callStart"] = nullptr;
    }
    if (e.callEnd.has_value()) {
        j["callEnd"] = formatLocalTimestamp(*e.callEnd);
    } else {
        j["callEnd"] = nullptr;
    }
    j["href"] = e.href;
    j["projectName"] = e.projectName;
    if (e.activeCallForViewer.has_value()) {
        j["activeCallForViewer"] = e.activeCallForViewer->v;
    } else {
        j["activeCallForViewer"] = nullptr;
    }
    auto others = nlohmann::json::array();
    for (const auto& u : e.otherActiveUsers) {
        others.push_back(u.v);
    }
    j["otherActiveUsers"] = std::move(others);
    j["description"] = e.description;
    // updatedAt rides inside the entry (REST + the WS ticket_upsert frame share
    // this projection) so the frontend can re-sort merged live deltas by the
    // server's "status rank → updatedAt desc → id" order. lockVersion is
    // deliberately NOT here — it rides at the WS frame top level (the version
    // guard), keeping the REST contract free of locking internals.
    j["updatedAt"] = formatUtcTimestamp(e.updatedAt);
    return j;
}

} // namespace aid::serialization
