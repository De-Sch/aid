# Header-only INTERFACE library. Pure virtual port interfaces; no .cpp files.
# Sits beneath aid_usecases / aid_controllers and is the ONLY aid_* target a
# plugin .so may link.
add_library(aid_ports INTERFACE)

target_include_directories(aid_ports
    INTERFACE ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_ports
    INTERFACE aid_value_types aid_plumbing
)
