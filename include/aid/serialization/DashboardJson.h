#pragma once

#include <nlohmann/json.hpp>

// Shared DashboardEntry → JSON projection. Lives in its own tiny target so the
// REST dashboard (controllers/UiController) and the live-delta WebSocket frame
// (adapters/ws/WsHubAdapter) emit a byte-identical `entry` object — the
// frontend has exactly one shape to parse. Timestamps render as the daemon's
// local wall-clock "YYYY-MM-DD HH:MM:SS", matching the stored ticket-system
// custom fields.

namespace aid {
struct DashboardEntry;
} // namespace aid

namespace aid::serialization {

[[nodiscard]] nlohmann::json toJson(const aid::DashboardEntry& entry);

} // namespace aid::serialization
