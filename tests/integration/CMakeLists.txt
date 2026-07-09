# tests/integration/ — end-to-end through Controller → Mailbox → UseCase → Port.
# Slow because each test spins up a trantor EventLoop on a background thread.
# Gated by the `slow;integration` label so `ctest -LE slow` skips them.

add_executable(aid_integration_tests
    it_incoming_call.cpp
    it_call_dispatch.cpp
    it_ui_round_trip.cpp
    it_wal_replay.cpp
    it_mailbox_backpressure.cpp
    it_sigterm_drain.cpp
    it_plugin_end_to_end.cpp
    it_query_scope.cpp
)

# it_plugin_end_to_end.cpp and it_sigterm_drain.cpp dlopen the real plugin .so
# files via PluginLoader, so the .so targets must be built first and their
# absolute paths handed in as compile-time macros (same pattern as
# tests/adapters/*/CMakeLists.txt).
#
# it_sigterm_drain.cpp's gated, "live"-labelled forked-daemon E2E test also
# forks the real daemon binary, so it needs that path too. The test self-skips unless AID_LIVE_E2E=1 AND the dev env is present.
add_dependencies(aid_integration_tests aid_openproject_plugin aid_davical_plugin aid)
target_compile_definitions(aid_integration_tests
    PRIVATE
        AID_OPENPROJECT_PLUGIN_PATH="$<TARGET_FILE:aid_openproject_plugin>"
        AID_DAVICAL_PLUGIN_PATH="$<TARGET_FILE:aid_davical_plugin>"
        AID_DAEMON_BINARY_PATH="$<TARGET_FILE:aid>"
)

target_link_libraries(aid_integration_tests
    PRIVATE
        aid_controllers
        aid_usecases
        aid_infrastructure
        aid_crosscutting
        aid_domain
        aid_ws_hub
        aid_test_fakes
        aid_warnings
        aid_sanitizers
        nlohmann_json::nlohmann_json
        GTest::gtest
        GTest::gtest_main
)

aid_register_test(TARGET aid_integration_tests LABELS "slow;integration")

# Dedicated "live"-labelled entry for the forked-daemon SIGTERM E2E test.
# It forks the real daemon against the dev env
# (OpenProject/DaviCal/auth.db per the running_daemon_live notes), so it is doubly
# gated and must never fail an unattended run:
#   * LABEL "live"      — selects WHICH test (run with `ctest -L live`);
#   * env AID_LIVE_E2E=1 — ARMS it. We deliberately do NOT set that env here, so
#     this entry (and the default-discovery copy above) self-SKIPS — exit 0,
#     ctest PASS — in any normal `ctest` / scripts/test.sh run.
# To actually run it against the dev env (after stopping any daemon already on
# the listen port — the WAL path is shared):
#   AID_LIVE_E2E=1 ctest --test-dir build -L live --output-on-failure
# It still self-skips if the daemon binary or config file is absent, so the
# armed form is a clean skip on a box without the dev env, never a failure.
add_test(
    NAME aid_sigterm_fork_e2e_live
    COMMAND $<TARGET_FILE:aid_integration_tests> --gtest_filter=SigtermForkE2E.*
)
set_tests_properties(aid_sigterm_fork_e2e_live PROPERTIES
    LABELS "live"
    TIMEOUT 120
)
