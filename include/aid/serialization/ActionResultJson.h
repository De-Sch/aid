#pragma once

#include <nlohmann/json.hpp>

// Shared ActionResult → JSON projection. The REST reply (controllers/
// UiController) dumps this object directly; the live WebSocket frame
// (adapters/ws/WsHubAdapter) wraps it in an {"type":"action_result", ...}
// envelope. Factored here so the two paths emit the same {ok, op, ticketId,
// message} fields — the frontend has exactly one ActionResult shape to parse,
// with no drift risk between the two call sites. `message` renders as JSON null
// (not "") when absent.

namespace aid::plumbing {
struct ActionResult;
} // namespace aid::plumbing

namespace aid::serialization {

[[nodiscard]] nlohmann::json toJson(const aid::plumbing::ActionResult& result);

} // namespace aid::serialization
