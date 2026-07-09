add_executable(aid main.cpp)

target_link_libraries(aid
    PRIVATE aid_value_types aid_plumbing
            aid_domain aid_ports
            aid_crosscutting aid_log_trantor
            aid_auth
            aid_ws_hub
            aid_usecases aid_controllers aid_infrastructure
            sodium                                   # ::sodium_init in main()
            aid_warnings aid_sanitizers
)

# aid-admin: standalone CLI for managing auth.db. Links aid_auth and
# crosscutting only — NO Drogon, NO controllers, NO infrastructure. The
# whole point is that operator user management does not pull in the
# HTTP framework.
add_executable(aid-admin admin/main.cpp)

target_link_libraries(aid-admin
    PRIVATE aid_auth
            aid_crosscutting
            aid_plumbing
            aid_value_types
            aid_warnings
            aid_sanitizers
)
