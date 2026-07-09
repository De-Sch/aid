# cmake/AidTest.cmake — shared ctest registration for GoogleTest executables.
#
# Every test executable registers with ctest identically, and every one needs
# the same LD_PRELOAD=libasan.so workaround under AID_SANITIZE: this host
# preloads NoMachine's libnxegl.so, which otherwise loads before libasan and
# abort()s gtest_discover_tests' build-time discovery launch. This factors that
# ~22-line block — previously copy-pasted into all 15 tests/**/CMakeLists.txt —
# into one helper. Companion to cmake/Sanitizers.cmake (the compile/link-flag
# half); include both once from the root CMakeLists so the function is visible
# in every add_subdirectory() scope.
#
# Usage:
#   aid_register_test(TARGET aid_foo_tests)
#   aid_register_test(TARGET aid_integration_tests LABELS "slow;integration")
#
# Behaviour is byte-for-byte what the hand-written blocks emitted: under
# AID_SANITIZE a single bundled add_test() runs the binary through `cmake -E
# env` with libasan preloaded and gtest discovery bypassed; otherwise
# gtest_discover_tests enumerates at ctest time (DISCOVERY_MODE PRE_TEST keeps
# the build phase fast). LABELS, when given, are applied to whichever entry the
# branch creates.
function(aid_register_test)
    cmake_parse_arguments(_art "" "TARGET" "LABELS" ${ARGN})
    if(NOT _art_TARGET)
        message(FATAL_ERROR "aid_register_test: TARGET is required")
    endif()

    if(AID_SANITIZE)
        execute_process(
            COMMAND ${CMAKE_CXX_COMPILER} -print-file-name=libasan.so
            OUTPUT_VARIABLE _art_libasan_path
            OUTPUT_STRIP_TRAILING_WHITESPACE
        )
        add_test(
            NAME ${_art_TARGET}_asan
            COMMAND ${CMAKE_COMMAND} -E env
                "LD_PRELOAD=${_art_libasan_path}"
                "ASAN_OPTIONS=halt_on_error=1:detect_leaks=1:print_stacktrace=1"
                "UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1"
                $<TARGET_FILE:${_art_TARGET}>
        )
        if(_art_LABELS)
            set_tests_properties(${_art_TARGET}_asan PROPERTIES LABELS "${_art_LABELS}")
        endif()
    else()
        include(GoogleTest)
        if(_art_LABELS)
            gtest_discover_tests(${_art_TARGET}
                DISCOVERY_MODE PRE_TEST
                PROPERTIES LABELS "${_art_LABELS}")
        else()
            gtest_discover_tests(${_art_TARGET} DISCOVERY_MODE PRE_TEST)
        endif()
    endif()
endfunction()
