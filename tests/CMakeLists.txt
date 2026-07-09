add_subdirectory(fakes)
add_subdirectory(integration)
add_subdirectory(value-types)
add_subdirectory(plumbing)
add_subdirectory(crosscutting)
add_subdirectory(auth)
add_subdirectory(domain)
add_subdirectory(ports)
add_subdirectory(infrastructure)
add_subdirectory(usecases)
add_subdirectory(controllers)
add_subdirectory(admin)
add_subdirectory(adapters/openproject_plugin)
add_subdirectory(adapters/davical_plugin)
add_subdirectory(adapters/ws)

add_executable(aid_tests
    test_smoke.cpp
)

target_link_libraries(aid_tests
    PRIVATE
        aid_crosscutting
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_tests)
