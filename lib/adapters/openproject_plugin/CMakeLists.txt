# OpenProject plugin internals — a STATIC library of helper sources
# (OpStatusMap, payload helpers, and later OpHttp/OpUserRepo/OpTicketRepo/
# OpDashboardBuilder). The plugin .so (MODULE library, added in commit 3)
# will link this whole library so the same code paths are exercised by
# the tests and by the loaded plugin.
#
# Layering note: per the approved feature plan, this plugin is a
# documented exception to the layering rules — it may link
# aid_infrastructure / aid_drogon PRIVATE so it can reuse HttpClient and
# Drogon's URL encoder. Headers exported from the .so still must not
# expose Drogon types; the extern "C" factory triplet in
# OpenProjectAdapter.cpp is the only public surface (commit 3).
#
# POSITION_INDEPENDENT_CODE ON so the same .o files can be linked into
# the MODULE library (commit 3) without recompiling.

add_library(aid_openproject_internals STATIC
    internal/url.cpp
    internal/OpStatusMap.cpp
    internal/payload.cpp
    internal/OpHttp.cpp
    internal/OpUserRepo.cpp
    internal/OpTicketRepo.cpp
    internal/OpDashboardBuilder.cpp
    internal/ProducedLedger.cpp
    internal/HandlerLedger.cpp
)

set_target_properties(aid_openproject_internals PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(aid_openproject_internals
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_openproject_internals
    PUBLIC
        aid_ports                                          # TicketStore — the interface the .so exports
        aid_value_types aid_plumbing                       # value-types + Result/Error/Task
        aid_crosscutting                                   # TicketSystemConfig, Logger
        aid_domain                                         # StateTransitions::path for closeTwoStep,
                                                            # CallLineFormatter for dashboard
        aid_infrastructure                                 # HttpClient + Headers + HttpResponse —
                                                            # documented exception to the layering rules, per
                                                            # the feature plan's "Decision A".
    PRIVATE
        aid_warnings aid_sanitizers
        nlohmann_json::nlohmann_json
)

# ─── Plugin .so itself ──────────────────────────────────────────────────
#
# aid_openproject_plugin (MODULE) — the dlopen-able shared object. Holds
# OpenProjectAdapter.cpp (the façade + the three extern "C" factory
# symbols) and pulls in the internals static library.
#
# PREFIX="" so the file is named aid_openproject_plugin.so, not lib…
# CXX_VISIBILITY_PRESET hidden so only the AID_PLUGIN_EXPORT-marked factory
# symbols (create_TicketStore, destroy_TicketStore, aid_plugin_api_version,
# aid_plugin_abi_layout_tag, aid_plugin_contract_tag) are externally visible.
# `nm -D --defined-only` on the built .so should turn up exactly those.
add_library(aid_openproject_plugin MODULE
    OpenProjectAdapter.cpp
)
set_target_properties(aid_openproject_plugin PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_libraries(aid_openproject_plugin
    PRIVATE
        aid_openproject_internals
        aid_warnings aid_sanitizers
        nlohmann_json::nlohmann_json
)
