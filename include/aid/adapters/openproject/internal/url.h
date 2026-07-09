#pragma once

#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

#include "aid/value-types/Ids.h"

// Shared URL / filter helpers used by every adapter source that builds
// OpenProject HAL URLs. Pulled out of OpTicketRepo.cpp + OpUserRepo.cpp
// + payload.cpp so the same RFC-3986 encoder and the same filter-JSON
// discipline are used everywhere.

namespace aid::adapters::openproject {

// RFC-3986 unreserved set: alpha, digit, '-', '_', '.', '~'. Every
// other byte is percent-encoded. Used to splice caller-supplied
// strings (callId, login, subject, phone) into URLs.
[[nodiscard]] std::string urlEncode(std::string_view s);

// Trailing path segment of a HAL href.
//   "/api/v3/users/42"          -> "42"
//   "/api/v3/projects/11/x/y"   -> "y"
// Empty input → empty string; no '/' → returns the input verbatim.
[[nodiscard]] std::string hrefTail(std::string_view href);

// "customField<id>" — every OpenProject custom field, regardless of
// data type, is keyed this way in HAL responses and POST/PATCH bodies.
[[nodiscard]] std::string customFieldName(const aid::CustomFieldId& id);

// Build a single-clause filter and splice it as `path?filters=<encoded>`.
// The `value` is escaped via nlohmann::json (quotes / backslashes /
// non-ASCII) and then urlEncoded — caller-supplied strings can contain
// any byte without breaking out of the operand. This is the injection-
// defence path; never hand-stitch a filter string.
[[nodiscard]] std::string singleFilterUrl(std::string_view path, std::string_view field,
                                          std::string_view op, std::string_view value);

// Multi-clause variant — caller builds the `filters` JSON array
// themselves (used for dashboard / find-latest queries that combine
// project + type + status clauses).
[[nodiscard]] std::string multiFilterUrl(std::string_view path, const nlohmann::json& filters);

} // namespace aid::adapters::openproject
