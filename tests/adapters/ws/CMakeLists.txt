add_executable(aid_ws_hub_tests
    test_ws_hub_adapter.cpp
)

target_link_libraries(aid_ws_hub_tests
    PRIVATE
        aid_ws_hub
        aid_test_fakes
        aid_warnings
        aid_sanitizers
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_ws_hub_tests)
