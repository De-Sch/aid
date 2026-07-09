# lib/usecases/ — one orchestrator per /call (and later /ui) action.
# Layering: no Drogon, no nlohmann::json, no infrastructure.

add_library(aid_usecases STATIC
    HandleIncomingCall.cpp
    HandleAcceptedCall.cpp
    HandleOutgoingCall.cpp
    HandleTransferCall.cpp
    HandleHangup.cpp
    CloseTicket.cpp
    AppendComment.cpp
    GetDashboard.cpp
    TicketDeltaEmitter.cpp
    ReconcileMemberships.cpp
)

target_include_directories(aid_usecases
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_usecases
    PUBLIC  aid_value_types aid_plumbing aid_domain aid_ports aid_crosscutting
    PRIVATE aid_warnings aid_sanitizers
)
