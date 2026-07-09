add_executable(aid_controllers_tests
    test_controller_support.cpp
    test_call_controller.cpp
    test_webhook_controller.cpp
    test_session_guard.cpp
    test_login_controller.cpp
    test_ui_controller.cpp
    test_ui_controller_loop_affinity.cpp
    test_ui_stream_controller.cpp
    test_health_controller.cpp
)

target_link_libraries(aid_controllers_tests
    PRIVATE
        aid_controllers
        aid_infrastructure
        aid_test_fakes
        aid_ws_hub
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_controllers_tests)
