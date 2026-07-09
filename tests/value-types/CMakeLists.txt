add_executable(aid_value_types_tests
    test_ids.cpp
    test_contact.cpp
    test_ticket.cpp
    test_callevent.cpp
    test_dashboard.cpp
)

target_link_libraries(aid_value_types_tests
    PRIVATE
        aid_value_types
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_value_types_tests)
