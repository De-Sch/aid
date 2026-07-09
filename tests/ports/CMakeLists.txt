add_executable(aid_ports_tests
    test_addressbook.cpp
    test_ticketstore.cpp
    test_uinotifier.cpp
)

target_link_libraries(aid_ports_tests
    PRIVATE
        aid_ports
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_ports_tests)
