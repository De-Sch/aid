add_executable(aid_davical_plugin_tests
    test_davical_adapter.cpp
    test_dc_http.cpp
    test_dc_vcard_parser.cpp
    test_plugin_smoke.cpp
)

# Smoke test dlopens the real plugin .so via PluginLoader's loop-aware
# overload, so we need the .so built before the tests run and we pass
# its absolute path in as a compile-time macro (same pattern as the
# OpenProject plugin tests).
add_dependencies(aid_davical_plugin_tests aid_davical_plugin)
target_compile_definitions(aid_davical_plugin_tests
    PRIVATE AID_DAVICAL_PLUGIN_PATH="$<TARGET_FILE:aid_davical_plugin>"
)

target_link_libraries(aid_davical_plugin_tests
    PRIVATE
        aid_davical_internals
        aid_infrastructure        # PluginLoader template + libdl
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_davical_plugin_tests)
