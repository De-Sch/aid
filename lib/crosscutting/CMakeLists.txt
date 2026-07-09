add_library(aid_crosscutting STATIC
    LogSink.cpp
    Logger.cpp
    Config.cpp
    CorrelationId.cpp
    Version.cpp
)

target_include_directories(aid_crosscutting
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

# aid::version() (aid/version.hpp) is implemented in Version.cpp; the version
# string is injected here as PROJECT_VERSION. Folded in from the retired
# aid_core placeholder target, which existed only to hold this one accessor.
target_compile_definitions(aid_crosscutting
    PRIVATE AID_VERSION_STRING="${PROJECT_VERSION}"
)

# aid_value_types is PUBLIC because Clock.h includes value-types/Ids.h
# for Timestamp; the header propagation needs to be explicit.
target_link_libraries(aid_crosscutting
    PUBLIC  aid_plumbing aid_value_types
    PRIVATE aid_warnings aid_sanitizers nlohmann_json::nlohmann_json
)

# Trantor bridge — separate target so aid_crosscutting itself stays
# Drogon-free per the layering rules. Defines Logger::routeTrantor only.
add_library(aid_log_trantor STATIC
    LoggerTrantorBridge.cpp
)

target_link_libraries(aid_log_trantor
    PRIVATE aid_crosscutting aid_drogon aid_warnings aid_sanitizers
)
