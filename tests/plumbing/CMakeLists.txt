add_executable(aid_plumbing_tests
    test_task.cpp
    test_result.cpp
    test_walrecord.cpp
    test_actionresult.cpp
)

target_link_libraries(aid_plumbing_tests
    PRIVATE
        aid_plumbing
        aid_warnings
        aid_sanitizers
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_plumbing_tests)
