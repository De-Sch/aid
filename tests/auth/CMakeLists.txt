add_executable(aid_auth_tests
    test_authdb.cpp
    test_password_hasher.cpp
    test_user_repo.cpp
    test_session_repo.cpp
    test_auth_service.cpp
    test_reset_grant_store.cpp
    test_user_gate.cpp
)

# The user/session/auth tests share tests/fakes/FakeClock.h (the consolidated
# test Clock). It's header-only, so we only need its directory on the include
# path — not a link against aid_test_fakes (which would drag in Drogon + the
# port fakes these tests don't use).
target_include_directories(aid_auth_tests
    PRIVATE ${CMAKE_SOURCE_DIR}/tests/fakes
)

# The AuthDb tests poke <sqlite3.h> directly (PRAGMA scalars,
# sqlite_master inspection) so they need the INTERFACE target. The
# library itself does not re-export sqlite3 (PRIVATE link).
target_link_libraries(aid_auth_tests
    PRIVATE
        aid_auth
        aid_warnings
        aid_sanitizers
        sqlite3
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_auth_tests)
