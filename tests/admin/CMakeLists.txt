add_executable(aid_admin_tests
    test_admin_cli.cpp
)

target_link_libraries(aid_admin_tests
    PRIVATE
        aid_auth
        aid_warnings
        aid_sanitizers
        sqlite3
        GTest::gtest
        GTest::gtest_main
)

# Tell the test harness where the aid-admin binary lives. The
# generator expression resolves at build time to the binary's path.
target_compile_definitions(aid_admin_tests
    PRIVATE AID_ADMIN_BINARY="$<TARGET_FILE:aid-admin>"
)

# Ensure aid-admin is built before the test binary so the path resolves.
add_dependencies(aid_admin_tests aid-admin)

aid_register_test(TARGET aid_admin_tests)
