#pragma once

#include <array>
#include <string>
#include <unordered_map>

#include "aid/value-types/Ids.h"

namespace aid::crosscutting {
// Forward-declared: only fromConfig() names it, by const reference. Pulling in
// the full Config.h (217 lines, transitively) here would land in every
// OpStatusMap.h consumer; the definition is included in OpStatusMap.cpp.
struct TicketSystemConfig;
} // namespace aid::crosscutting

// OpStatusMap — bidirectional TicketStatus ↔ OpenProject StatusId.
//
// Built once in Main from the config slice (cfg.statusNew, statusInProgress,
// statusClosed). After construction the map is
// read-only and lock-free; hrefIdFor() and statusFor() are pure lookups.

namespace aid::adapters::openproject {

class OpStatusMap {
public:
    [[nodiscard]] static OpStatusMap fromConfig(const aid::crosscutting::TicketSystemConfig& cfg);

    // Used in PATCH payloads to build "/api/v3/statuses/<id>". Never
    // throws (the .so boundary must not see exceptions). On the
    // defensive case where a slot
    // was left empty by fromConfig (operator misconfigured an empty
    // status string), returns an empty StatusId and emits a WARN —
    // the resulting "/api/v3/statuses/" URL will be rejected by
    // OpenProject with a 4xx, which propagates as a normal Error.
    [[nodiscard]] aid::StatusId hrefIdFor(aid::TicketStatus s) const noexcept;

    // Reverse lookup used by parseFromHal. Unknown id → TicketStatus::New
    // and emits a defensive WARN to Logger; should never happen in practice
    // because every status id surfaced by OpenProject is one we configured.
    // noexcept like hrefIdFor — every code path inside the plugin .so must
    // be safe to call from coroutines invoked across the extern "C"
    // boundary (never throws across the .so).
    [[nodiscard]] aid::TicketStatus statusFor(const aid::StatusId& id) const noexcept;

    // Number of TicketStatus values. Kept in sync with the enum in Ids.h:
    // New, InProgress, Closed.
    static constexpr std::size_t kStatusCount = 3;

private:
    std::array<aid::StatusId, kStatusCount> forward_{};
    std::unordered_map<std::string, aid::TicketStatus> reverse_; // keyed by StatusId.v
};

} // namespace aid::adapters::openproject
