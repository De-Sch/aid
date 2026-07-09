add_executable(aid_crosscutting_tests
    test_logsink.cpp
    test_logger.cpp
    test_clock.cpp
    test_correlationid.cpp
    test_config.cpp
    test_config_routing.cpp
)

target_link_libraries(aid_crosscutting_tests
    PRIVATE
        aid_crosscutting
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_crosscutting_tests)
