add_executable(aid_domain_tests
    test_calllineformatter.cpp
    test_calltracker.cpp
    test_statetransitions.cpp
    test_ticketrouter.cpp
)

target_link_libraries(aid_domain_tests
    PRIVATE
        aid_domain
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_domain_tests)
