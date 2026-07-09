# lib/adapters/ws/ — in-process UiNotifier implementation that holds live
# WebSocket connections. This is NOT a plugin .so: it's a regular static
# library that the daemon executable links directly, exempt from the
# plugin-ABI no-Drogon constraint because callers (UiStreamController +
# the daemon's use-cases via UiNotifier&) are in the same process.

add_library(aid_ws_hub STATIC
    WsHubAdapter.cpp
)

target_include_directories(aid_ws_hub
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_ws_hub
    PUBLIC  aid_ports aid_value_types aid_plumbing aid_crosscutting aid_drogon
    PRIVATE aid_warnings aid_sanitizers nlohmann_json::nlohmann_json aid_serialization
)
