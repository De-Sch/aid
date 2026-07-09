add_executable(aid_usecases_tests
    test_handle_incoming_call.cpp
    test_handle_accepted_call.cpp
    test_handle_outgoing_call.cpp
    test_handle_transfer_call.cpp
    test_handle_hangup.cpp
    test_close_ticket.cpp
    test_append_comment.cpp
    test_concurrent_same_ticket.cpp
    test_get_dashboard.cpp
    test_ticket_delta_emitter.cpp
    test_reconcile_memberships.cpp
)

target_link_libraries(aid_usecases_tests
    PRIVATE
        aid_usecases
        aid_test_fakes
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_usecases_tests)
