add_library(aid_value_types STATIC
    CallEvent.cpp
    TimeFormat.cpp
)

target_include_directories(aid_value_types
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_value_types
    PRIVATE aid_warnings aid_sanitizers
)
