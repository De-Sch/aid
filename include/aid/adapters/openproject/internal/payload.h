#pragma once

#include <nlohmann/json_fwd.hpp>
#include <optional>
#include <string>
#include <vector>

#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Result.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

// Payload helpers — the single JSON ↔ domain edge for the OpenProject
// plugin. Every HAL-shaped response funnels through parseFromHal();
// every POST/PATCH body is built by toCreatePayload() / toPatchPayload().
// Keeping these as free functions (no class state) makes them trivially
// testable from outside the plugin, and isolates the entire OpenProject
// JSON contract to one source file.
//
// Errors that indicate a malformed HAL body (missing required field,
// wrong type, unparseable timestamp) return ErrorCode::InvalidInput so
// the adapter can surface a useful error to the use case without crashing.

namespace aid::adapters::openproject {

[[nodiscard]] aid::plumbing::Result<aid::Ticket>
parseFromHal(const nlohmann::json& hal, const CustomFieldMap& fields, const OpStatusMap& statusMap);

[[nodiscard]] nlohmann::json
toCreatePayload(const aid::NewTicket& nt, const CustomFieldMap& fields,
                const OpStatusMap& statusMap, const aid::crosscutting::TicketSystemConfig& cfg,
                const std::optional<std::string>& assigneeHref = std::nullopt);

[[nodiscard]] nlohmann::json
toPatchPayload(const aid::Ticket& t, const CustomFieldMap& fields, const OpStatusMap& statusMap,
               const std::optional<std::string>& assigneeHref = std::nullopt);

// Minimal PATCH body that touches ONLY the callHandler field (plus the
// mandatory lockVersion). OpTicketRepo::addCallHandler owns the callHandler
// custom field exclusively, merging logins under its own refetch→union→patch
// 409 loop; toPatchPayload deliberately leaves the field alone so a plain
// status/assignee save can never clobber a concurrently-recorded handler.
// `handlers` is written as the ", "-separated login CSV parseFromHal reads back.
[[nodiscard]] nlohmann::json toCallHandlerPatch(int lockVersion,
                                                const std::vector<aid::UserHandle>& handlers,
                                                const CustomFieldMap& fields);

} // namespace aid::adapters::openproject
