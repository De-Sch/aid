add_library(aid_plumbing STATIC
    Error.cpp
)

target_include_directories(aid_plumbing
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_plumbing
    PUBLIC  aid_value_types
    PRIVATE aid_warnings aid_sanitizers
)
