add_executable(aid_openproject_plugin_tests
    test_op_status_map.cpp
    test_payload.cpp
    test_op_http.cpp
    test_op_user_repo.cpp
    test_op_ticket_repo.cpp
    test_op_dashboard.cpp
    test_produced_ledger.cpp
    test_handler_ledger.cpp
    test_plugin_smoke.cpp
)

# The smoke test dlopens the real plugin .so via PluginLoader's loop-aware
# overload, so we need the .so built before the tests run and we pass its
# absolute path in as a compile-time macro (same pattern as
# tests/infrastructure/ for its test plugin).
add_dependencies(aid_openproject_plugin_tests aid_openproject_plugin)
target_compile_definitions(aid_openproject_plugin_tests
    PRIVATE AID_OPENPROJECT_PLUGIN_PATH="$<TARGET_FILE:aid_openproject_plugin>"
)

# Test sources need to #include "tests/adapters/openproject_plugin/fake_http_dispatcher.h",
# so make the project root visible (mirrors how tests/infrastructure/ wires its
# private test plugin header).
target_include_directories(aid_openproject_plugin_tests
    PRIVATE ${CMAKE_SOURCE_DIR}
)

# nlohmann_json::nlohmann_json is needed PRIVATE because the test sources
# parse / build JSON directly via nlohmann::json. The static internals
# library (aid_openproject_internals) re-exports the value-types +
# crosscutting headers via PUBLIC link so this target also gets the
# TicketSystemConfig / TicketStatus types it asserts against.
target_link_libraries(aid_openproject_plugin_tests
    PRIVATE
        aid_openproject_internals
        aid_infrastructure                                  # PluginLoader (header-only template,
                                                             # but its dlopen path needs libdl which
                                                             # aid_infrastructure carries PUBLIC)
        aid_warnings
        aid_sanitizers
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_openproject_plugin_tests)
