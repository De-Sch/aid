# Tiny test-only plugin .so for PluginLoader. Lives as a MODULE library so
# CMake doesn't try to link other targets against it (it's dlopen'd at
# runtime, not link-time). Same PREFIX="" trick as production plugins so the
# file is named aid_test_plugin.so, not libaid_test_plugin.so.
add_library(aid_test_plugin MODULE
    test_plugin/test_plugin.cpp
)
set_target_properties(aid_test_plugin PROPERTIES PREFIX "")
target_link_libraries(aid_test_plugin
    PRIVATE
        aid_ports # header-only INTERFACE: pulls value-types/plumbing for PluginAbiTag.h
        aid_warnings
        aid_sanitizers
)

add_executable(aid_infrastructure_tests
    test_wal.cpp
    test_mailbox.cpp
    test_webhook_mailbox.cpp
    test_plugin_loader.cpp
    test_http_client.cpp
    test_health_service.cpp
    test_startup_sequencer.cpp
    test_membership_reconciler.cpp
)

# PluginLoader test needs the .so before run-time; ensure CMake orders it.
add_dependencies(aid_infrastructure_tests aid_test_plugin)

# The PluginLoader test pulls in test_plugin/test_port.h to declare the
# port the .so exports. Visible only to this test target.
target_include_directories(aid_infrastructure_tests
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
)

target_compile_definitions(aid_infrastructure_tests
    PRIVATE AID_TEST_PLUGIN_PATH="$<TARGET_FILE:aid_test_plugin>"
)

target_link_libraries(aid_infrastructure_tests
    PRIVATE
        aid_infrastructure
        aid_test_fakes
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_infrastructure_tests)
