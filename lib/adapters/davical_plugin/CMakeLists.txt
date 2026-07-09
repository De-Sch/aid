# DaviCal plugin internals — STATIC library of helpers (DcHttp,
# DcVCardParser). The MODULE library at the bottom of this file links
# these into the dlopen-able .so.
#
# Layering exception: like OpenProject,
# DaviCal may PRIVATE-link aid_infrastructure (for HttpClient), aid_drogon
# (for Drogon types behind that), and the value-types stack. No Drogon
# types ever cross the extern "C" boundary — the only public surface is
# the three AID_PLUGIN_EXPORT symbols in DaviCalAdapter.cpp.
#
# POSITION_INDEPENDENT_CODE ON so the same .o files can be linked into
# the MODULE library; CXX_VISIBILITY_PRESET hidden so internal symbols
# stay invisible to dlsym() callers.

add_library(aid_davical_internals STATIC
    DaviCalAdapter.cpp
    internal/DcHttp.cpp
    internal/DcVCardParser.cpp
)

set_target_properties(aid_davical_internals PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

target_include_directories(aid_davical_internals
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

target_link_libraries(aid_davical_internals
    PUBLIC
        # PUBLIC = types leak through this lib's headers, so any
        # consumer (the MODULE .so + the test exe) also sees the dep.
        aid_ports                     # AddressBook abstract base
        aid_value_types aid_plumbing  # Contact, PhoneNumber, Result, Task
        aid_infrastructure            # HttpClient& in DcHttp.h — documented
                                       # layering exception
    PRIVATE
        # PRIVATE = consumed only inside this lib's .cpp files.
        # aid_crosscutting (Logger) is used by DaviCalAdapter.cpp only;
        # nlohmann_json parses the config_json inside the same file.
        # LibXml2 parses the CardDAV multistatus/vCard responses
        # (DcVCardParser.cpp); phonenumber canonicalizes TEL values for
        # matching (DaviCalAdapter.cpp). Both are stateless to link against.
        aid_warnings aid_sanitizers
        aid_crosscutting
        LibXml2::LibXml2              # vCard multistatus parsing
        phonenumber                   # phone-number canonicalize()
        nlohmann_json::nlohmann_json
)

# ─── Plugin .so itself ──────────────────────────────────────────────────
#
# aid_davical_plugin (MODULE) — the dlopen-able shared object. Carries
# DaviCalAdapter.cpp (the facade + the three extern "C" factory
# symbols) and pulls in the internals static library.
#
# PREFIX="" → file name is aid_davical_plugin.so, not lib…
# CXX_VISIBILITY_PRESET hidden + AID_PLUGIN_EXPORT on each factory
# symbol → only create_AddressBook / destroy_AddressBook /
# aid_plugin_api_version / aid_plugin_abi_layout_tag /
# aid_plugin_contract_tag are externally visible.

add_library(aid_davical_plugin MODULE
    factory.cpp
)
set_target_properties(aid_davical_plugin PROPERTIES
    PREFIX ""
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)
target_link_libraries(aid_davical_plugin
    PRIVATE
        aid_davical_internals
        aid_warnings aid_sanitizers
        nlohmann_json::nlohmann_json
)
