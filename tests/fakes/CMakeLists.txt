# tests/fakes/ — port test doubles (FakeTicketStore, FakeAddressBook,
# FakeUiNotifier, FakeClock). Implements `aid_test_fakes` as a STATIC library
# linked by any unit/integration test that wants to drive a use case or
# controller without real adapter .so's.

add_library(aid_test_fakes STATIC
    FakeTicketStore.cpp
    FakeAddressBook.cpp
    FakeUiNotifier.cpp
    FakeWebSocketConnection.cpp
)

target_include_directories(aid_test_fakes
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(aid_test_fakes
    PUBLIC  aid_ports aid_value_types aid_plumbing aid_crosscutting aid_drogon
    PRIVATE aid_warnings aid_sanitizers
)
