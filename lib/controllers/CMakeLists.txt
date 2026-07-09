# lib/controllers/ — Drogon HTTP/WS entry points. Layering:
# may link Drogon and nlohmann::json; uses Mailbox/Wal via constructor-DI (headers
# included in the .cpp; symbols resolve at executable/test-exe link time —
# aid_controllers itself does not list aid_infrastructure as a CMake link).

add_library(aid_controllers STATIC
    ControllerSupport.cpp
    CallController.cpp
    WebhookController.cpp
    SessionGuard.cpp
    LoginController.cpp
    UiController.cpp
    UiStreamController.cpp
    HealthController.cpp
)

target_include_directories(aid_controllers
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_controllers
    PUBLIC  aid_usecases aid_value_types aid_plumbing aid_crosscutting aid_drogon aid_auth aid_ws_hub aid_infrastructure
    PRIVATE aid_warnings aid_sanitizers nlohmann_json::nlohmann_json aid_serialization
)
