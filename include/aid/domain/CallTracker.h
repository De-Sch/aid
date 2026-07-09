#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "aid/value-types/Ids.h"

namespace aid::domain {

// Encoding of the `callId` custom field on a ticket-system ticket: a
// comma-separated list of zero, one, or many active Asterisk Uniqueids.
// All methods are pure, static, no I/O.
class CallTracker {
public:
    [[nodiscard]] static std::vector<CallId> decode(std::string_view field);
    [[nodiscard]] static std::string encode(std::span<const CallId> ids);
    [[nodiscard]] static bool contains(std::string_view field, const CallId& id);
    [[nodiscard]] static std::string withAdded(std::string_view field, const CallId& id);
    [[nodiscard]] static std::string withRemoved(std::string_view field, const CallId& id);
};

} // namespace aid::domain
