add_library(aid_domain STATIC
    CallLineFormatter.cpp
    CallTracker.cpp
    StateTransitions.cpp
    TicketRouter.cpp
)

target_include_directories(aid_domain
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_domain
    PUBLIC  aid_value_types aid_plumbing
    PRIVATE aid_warnings aid_sanitizers
)
