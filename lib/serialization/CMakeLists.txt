# lib/serialization/ — edge JSON projections shared by the REST controllers and
# the WebSocket hub so both emit a byte-identical `entry` shape. Layering: speaks
# value-types + nlohmann::json only (no Drogon, no usecases).

add_library(aid_serialization STATIC
    DashboardJson.cpp
    ActionResultJson.cpp
)

target_include_directories(aid_serialization
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_serialization
    # aid_plumbing: ActionResultJson.h projects aid::plumbing::ActionResult.
    PUBLIC  aid_value_types aid_plumbing nlohmann_json::nlohmann_json
    PRIVATE aid_warnings aid_sanitizers
)
