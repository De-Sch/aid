add_library(aid_infrastructure STATIC
    Wal.cpp
    MailboxEngine.cpp
    Mailbox.cpp
    WebhookMailbox.cpp
    HttpClient.cpp
    HealthService.cpp
    MembershipReconciler.cpp
)

target_include_directories(aid_infrastructure
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

# WalRecord, Result, Clock leak through Wal.h → PUBLIC propagation.
# nlohmann_json is implementation-only (line (de)serialization) and must NOT
# transit into consumers, since plumbing/ forbids it.
# Mailbox.h forward-declares trantor::EventLoop in its API, so consumers
# constructing a Mailbox need trantor — aid_drogon (Drogon's SYSTEM-include
# wrapper) is PUBLIC. Allowed by the layering rules for "aid_infrastructure".
# ${CMAKE_DL_LIBS} is PUBLIC because PluginLoader.h's template body calls
# dlopen/dlsym inline; every consumer that instantiates PluginLoader<Port>
# needs libdl symbols at link time.
target_link_libraries(aid_infrastructure
    PUBLIC  aid_plumbing aid_crosscutting aid_value_types aid_ports aid_drogon ${CMAKE_DL_LIBS}
    PRIVATE aid_warnings aid_sanitizers nlohmann_json::nlohmann_json
)
