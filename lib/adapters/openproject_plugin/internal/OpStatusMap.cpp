#include "aid/adapters/openproject/internal/OpStatusMap.h"

#include <cstddef>
#include <string>

#include "aid/crosscutting/Config.h" // full TicketSystemConfig (fwd-declared in the header)
#include "aid/crosscutting/Logger.h"

namespace aid::adapters::openproject {

namespace {

constexpr std::size_t idx(aid::TicketStatus s) noexcept {
    return static_cast<std::size_t>(s);
}

} // namespace

OpStatusMap OpStatusMap::fromConfig(const aid::crosscutting::TicketSystemConfig& cfg) {
    OpStatusMap m;
    m.forward_[idx(aid::TicketStatus::New)] = cfg.statusNew;
    m.forward_[idx(aid::TicketStatus::InProgress)] = cfg.statusInProgress;
    m.forward_[idx(aid::TicketStatus::Closed)] = cfg.statusClosed;

    m.reverse_.emplace(cfg.statusNew.v, aid::TicketStatus::New);
    m.reverse_.emplace(cfg.statusInProgress.v, aid::TicketStatus::InProgress);
    m.reverse_.emplace(cfg.statusClosed.v, aid::TicketStatus::Closed);
    return m;
}

aid::StatusId OpStatusMap::hrefIdFor(aid::TicketStatus s) const noexcept {
    const auto i = idx(s);
    if (i >= kStatusCount) {
        // Defensive: a new TicketStatus enum value was added without
        // updating fromConfig. Unreachable in correct code; surface as a
        // loud log and an empty href that OpenProject will 4xx on.
        // Throwing here would escape the plugin .so boundary.
        aid::crosscutting::Logger::instance().warn(
            "OpStatusMap::hrefIdFor: TicketStatus value " + std::to_string(i) +
                " is out of range — fromConfig is missing a slot",
            aid::crosscutting::LogType::BACKEND);
        return aid::StatusId{};
    }
    const auto& id = forward_[i];
    if (id.v.empty()) {
        // Operator misconfigured this status as an empty string. Same
        // defensive treatment: log and return empty so the URL the
        // adapter builds is malformed (OpenProject returns 4xx, the
        // daemon sees a clean Error).
        aid::crosscutting::Logger::instance().warn(
            "OpStatusMap::hrefIdFor: TicketStatus " + std::to_string(i) +
                " has no configured status id (check config.json)",
            aid::crosscutting::LogType::BACKEND);
        return aid::StatusId{};
    }
    return id;
}

aid::TicketStatus OpStatusMap::statusFor(const aid::StatusId& id) const noexcept {
    auto it = reverse_.find(id.v);
    if (it != reverse_.end()) {
        return it->second;
    }
    // Defensive: every status surfaced by OpenProject should be one we
    // configured. If not, we degrade to New rather than crashing the
    // entire mailbox event. The WARN line tells the operator something
    // about the OpenProject workflow drifted from config.
    aid::crosscutting::Logger::instance().warn(
        "OpStatusMap::statusFor: unknown OpenProject status id '" + id.v +
            "', defaulting to TicketStatus::New",
        aid::crosscutting::LogType::BACKEND);
    return aid::TicketStatus::New;
}

} // namespace aid::adapters::openproject
