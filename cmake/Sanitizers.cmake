function(aid_enable_sanitizers target)
    if(NOT AID_SANITIZE)
        return()
    endif()

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set(_aid_san_flags
            -fsanitize=address
            -fsanitize=undefined
            -fno-omit-frame-pointer
            -fno-sanitize-recover=all
        )
        target_compile_options(${target} INTERFACE ${_aid_san_flags})
        target_link_options(${target} INTERFACE ${_aid_san_flags})
        message(STATUS "Sanitizers enabled on ${target}: ASan + UBSan")
        # NOTE: tests run via ctest still need LD_PRELOAD=libasan.so to defeat
        # any system-injected .so (e.g. NoMachine's libnxegl.so) that would
        # otherwise load before libasan. scripts/sanitize.sh handles that.
    else()
        message(WARNING "AID_SANITIZE requested but compiler ${CMAKE_CXX_COMPILER_ID} not supported")
    endif()
endfunction()
