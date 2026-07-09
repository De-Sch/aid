cmake_minimum_required(VERSION 3.20)
project(aid VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Plugin .so's (aid_openproject_plugin, future aid_davical_plugin) link
# the static aid_* + Drogon archives into a shared module. Every object
# that ends up inside a .so must be position-independent, otherwise the
# linker rejects `R_X86_64_TPOFF32` and similar relocations. We pay the
# (tiny) PIC overhead on the daemon executable too — the alternative is
# duplicating every static target as PIC + non-PIC.
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "Build type" FORCE)
endif()

option(AID_SANITIZE   "Enable ASan + UBSan"           OFF)
option(AID_WERROR     "Treat warnings as errors"      ON)
option(AID_BUILD_TESTS "Build the GoogleTest suite"   ON)

add_library(aid_warnings INTERFACE)
target_compile_options(aid_warnings INTERFACE
    -Wall -Wextra -Wpedantic
    -Wshadow -Wnon-virtual-dtor -Wold-style-cast
    -Wcast-align -Wunused -Woverloaded-virtual
    -Wconversion -Wsign-conversion -Wnull-dereference
    -Wdouble-promotion -Wformat=2
)
if(AID_WERROR)
    target_compile_options(aid_warnings INTERFACE -Werror)
endif()

include(cmake/Sanitizers.cmake)
add_library(aid_sanitizers INTERFACE)
aid_enable_sanitizers(aid_sanitizers)

# Shared ctest registration for the test executables (aid_register_test).
# Defined here so it is visible in every tests/ add_subdirectory() scope.
include(cmake/AidTest.cmake)

include(FetchContent)

# Drogon — HTTP/WS + async client + C++20 coroutines.
# BUILD_SSL=ON: TLS for OpenProject/DaviCal upstreams.
# ORM/DB backends off: we have no SQL state of our own.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING     OFF CACHE BOOL "" FORCE)
set(BUILD_EXAMPLES    OFF CACHE BOOL "" FORCE)
set(BUILD_CTL         OFF CACHE BOOL "" FORCE)
set(BUILD_SSL         ON  CACHE BOOL "" FORCE)
set(BUILD_BROTLI      OFF CACHE BOOL "" FORCE)
set(BUILD_ORM         OFF CACHE BOOL "" FORCE)
set(BUILD_POSTGRESQL  OFF CACHE BOOL "" FORCE)
set(BUILD_MYSQL       OFF CACHE BOOL "" FORCE)
set(BUILD_SQLITE      OFF CACHE BOOL "" FORCE)
set(BUILD_REDIS       OFF CACHE BOOL "" FORCE)
# Drogon v1.9.13 ships PROPFIND/MKCOL/COPY/MOVE but not REPORT (RFC 3253).
# DaviCal's address-book queries are REPORT-only, so we patch the enum +
# the three switch tables that serialise the method line. The patch is
# pinned next to the GIT_TAG (`cmake/patches/drogon-add-report-verb.patch`,
# 13 +/- lines across two files).
#
# Idempotency without masking errors: `grep -q 'Report,'` returns 0 if
# the enum has already been patched, in which case we skip. If it's
# missing, we apply — and a real patch failure (drift after a Drogon
# bump) propagates non-zero so CMake configure fails loudly rather
# than silently producing an unpatched build that link-errors with a
# cryptic `HttpMethod::Report not declared` later.
FetchContent_Declare(
    drogon
    GIT_REPOSITORY https://github.com/drogonframework/drogon.git
    GIT_TAG        v1.9.13
    GIT_SHALLOW    TRUE
    PATCH_COMMAND  bash -c
                   "grep -q 'Report,' lib/inc/drogon/HttpTypes.h || \
                    patch -p1 -i '${CMAKE_SOURCE_DIR}/cmake/patches/drogon-add-report-verb.patch'"
)
FetchContent_MakeAvailable(drogon)

# nlohmann/json — header-only JSON parser used at adapter/edge sites.
# Lives in the crosscutting layer (Config) and later in JSON-edge code;
# kept out of public headers via pimpl so consumers don't pay the include.
set(JSON_BuildTests OFF CACHE BOOL "" FORCE)
set(JSON_Install    OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    nlohmann_json
    GIT_REPOSITORY https://github.com/nlohmann/json.git
    GIT_TAG        v3.11.3
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(nlohmann_json)

# SYSTEM-include Drogon so -Werror -Wconversion -Wshadow stay strict on AID
# code without firing on third-party. Layering rules restrict who may link this.
add_library(aid_drogon INTERFACE)
target_link_libraries(aid_drogon INTERFACE drogon)
get_target_property(_drogon_inc drogon INTERFACE_INCLUDE_DIRECTORIES)
if(_drogon_inc)
    target_include_directories(aid_drogon SYSTEM INTERFACE ${_drogon_inc})
endif()

# libxml2 — vCard multistatus parser for the DaviCal plugin. XXE attack
# surface lives in the parser, not the library: we run with NONET +
# without DTDLOAD + without NOENT, and override the external-entity
# loader to a no-op. See lib/adapters/davical_plugin/internal/DcVCardParser.cpp.
# Static build keeps the .so self-contained at the deploy root's plugins/ dir.
set(LIBXML2_WITH_PYTHON    OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_TESTS     OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_PROGRAMS  OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_HTTP      OFF CACHE BOOL "" FORCE) # XXE-hardening: no net.
set(LIBXML2_WITH_FTP       OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_ICONV     OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_LZMA      OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_ZLIB      OFF CACHE BOOL "" FORCE)
set(LIBXML2_WITH_MODULES   OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    libxml2
    GIT_REPOSITORY https://gitlab.gnome.org/GNOME/libxml2.git
    GIT_TAG        v2.13.5
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(libxml2)

# libphonenumber-cpp — single authority on "is this a phone number?"
#
# DEVIATION from "FetchContent + pinned tag" (the usual default here):
# the upstream repo at google/libphonenumber pulls
# protobuf + ICU + abseil via opinionated find_package calls, and
# FetchContent_Declare(... GIT_TAG v8.13.50) clones every ref at depth 1
# (--no-single-branch is unavoidable when GIT_TAG isn't a branch HEAD),
# which on this network stalls at the index-pack step for >10 min. The
# pragmatic fix: pin the Debian package (libphonenumber-dev 8.12.57+ds-3
# on the dev box, which mirrors prod) and wrap it as a thin INTERFACE
# target named `phonenumber` so consumers link it exactly as if we'd
# FetchContent'd. The version drift (8.13.50 → 8.12.57) is well within
# the 8.x API-stable range; canonicalize() only uses Parse +
# IsPossibleNumber + Format(E164), all stable since 8.0.
find_library(AID_PHONENUMBER_LIB phonenumber REQUIRED)
find_path(AID_PHONENUMBER_INC phonenumbers/phonenumberutil.h REQUIRED)

# try_compile so a contributor whose box lacks libphonenumber-dev fails
# at configure time with a directed message rather than at link time
# with `undefined reference to phonenumbers::PhoneNumberUtil::*`. The
# probe exercises exactly the three calls canonicalize() uses, so a
# future libphonenumber major bump that removes any of them also fails
# here — keeping the configure-time signal sharp.
try_compile(AID_PHONENUMBER_API_OK
    "${CMAKE_BINARY_DIR}/cmake-probe-phonenumber"
    SOURCES "${CMAKE_SOURCE_DIR}/cmake/probes/phonenumber_api.cpp"
    LINK_LIBRARIES "${AID_PHONENUMBER_LIB}"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${AID_PHONENUMBER_INC}"
    CXX_STANDARD 20
    OUTPUT_VARIABLE AID_PHONENUMBER_PROBE_LOG
)
if(NOT AID_PHONENUMBER_API_OK)
    message(FATAL_ERROR
        "libphonenumber Parse/IsPossibleNumber/Format API probe failed.\n"
        "Found library: ${AID_PHONENUMBER_LIB}\n"
        "Found include: ${AID_PHONENUMBER_INC}\n"
        "Install: sudo apt install libphonenumber-dev (≥ 8.x).\n"
        "Probe build output:\n${AID_PHONENUMBER_PROBE_LOG}")
endif()

add_library(phonenumber INTERFACE)
target_include_directories(phonenumber SYSTEM INTERFACE ${AID_PHONENUMBER_INC})
target_link_libraries(phonenumber INTERFACE ${AID_PHONENUMBER_LIB})

# libsqlite3 — local auth DB.
#
# Same documented-exception pattern as libphonenumber above: APT-pinned
# rather than FetchContent. Reasons specific to SQLite:
#   - canonical distribution is the amalgamation .c blob, not a CMake
#     project — FetchContent gains nothing over the system package on
#     reproducibility, and loses out on APT's security updates.
#   - rock-solid ABI; a min-version floor is all the pinning we need.
#   - matches the prod Debian box (3.40+).
#
# The probe exercises every symbol + flag aid_auth touches, so a future
# major bump that removes any of them fails configure with a directed
# message rather than at link time. SQLITE_OPEN_NOFOLLOW (added in 3.31)
# sets the effective floor.
find_library(AID_SQLITE3_LIB sqlite3 REQUIRED)
find_path(AID_SQLITE3_INC sqlite3.h REQUIRED)

try_compile(AID_SQLITE3_API_OK
    "${CMAKE_BINARY_DIR}/cmake-probe-sqlite3"
    SOURCES "${CMAKE_SOURCE_DIR}/cmake/probes/sqlite3_api.cpp"
    LINK_LIBRARIES "${AID_SQLITE3_LIB}"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${AID_SQLITE3_INC}"
    CXX_STANDARD 20
    OUTPUT_VARIABLE AID_SQLITE3_PROBE_LOG
)
if(NOT AID_SQLITE3_API_OK)
    message(FATAL_ERROR
        "libsqlite3 open_v2/prepare_v2/exec API probe failed.\n"
        "Found library: ${AID_SQLITE3_LIB}\n"
        "Found include: ${AID_SQLITE3_INC}\n"
        "Install: sudo apt install libsqlite3-dev (>= 3.40, for SQLITE_OPEN_NOFOLLOW).\n"
        "Probe build output:\n${AID_SQLITE3_PROBE_LOG}")
endif()

add_library(sqlite3 INTERFACE)
target_include_directories(sqlite3 SYSTEM INTERFACE ${AID_SQLITE3_INC})
target_link_libraries(sqlite3 INTERFACE ${AID_SQLITE3_LIB})

# libsodium — Argon2id + randombytes.
#
# Same APT-pinned pattern. Specific to libsodium:
#   - upstream uses autotools, not CMake; FetchContent would require a
#     third-party CMake wrapper repo, adding a maintained-by-someone-else
#     layer to a security-critical dependency.
#   - APT delivers CVE updates automatically.
#   - libsodium has a strict no-breakage-of-stable-API policy, so a
#     min-version floor is sufficient.
#
# The probe exercises sodium_init + the four crypto_pwhash_* calls +
# randombytes_buf + sodium_memzero. Floor is 1.0.18 (Debian bookworm).
find_library(AID_SODIUM_LIB sodium REQUIRED)
find_path(AID_SODIUM_INC sodium.h REQUIRED)

try_compile(AID_SODIUM_API_OK
    "${CMAKE_BINARY_DIR}/cmake-probe-sodium"
    SOURCES "${CMAKE_SOURCE_DIR}/cmake/probes/sodium_api.cpp"
    LINK_LIBRARIES "${AID_SODIUM_LIB}"
    CMAKE_FLAGS "-DINCLUDE_DIRECTORIES=${AID_SODIUM_INC}"
    CXX_STANDARD 20
    OUTPUT_VARIABLE AID_SODIUM_PROBE_LOG
)
if(NOT AID_SODIUM_API_OK)
    message(FATAL_ERROR
        "libsodium crypto_pwhash_str/verify API probe failed.\n"
        "Found library: ${AID_SODIUM_LIB}\n"
        "Found include: ${AID_SODIUM_INC}\n"
        "Install: sudo apt install libsodium-dev (>= 1.0.18).\n"
        "Probe build output:\n${AID_SODIUM_PROBE_LOG}")
endif()

add_library(sodium INTERFACE)
target_include_directories(sodium SYSTEM INTERFACE ${AID_SODIUM_INC})
target_link_libraries(sodium INTERFACE ${AID_SODIUM_LIB})

add_subdirectory(lib)
add_subdirectory(src)

if(AID_BUILD_TESTS)
    enable_testing()
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(
        googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG        v1.15.2
    )
    FetchContent_MakeAvailable(googletest)
    add_subdirectory(tests)
endif()

message(STATUS "AID2.0 ${PROJECT_VERSION} | C++${CMAKE_CXX_STANDARD} | ${CMAKE_BUILD_TYPE} | "
               "sanitize=${AID_SANITIZE} | werror=${AID_WERROR} | tests=${AID_BUILD_TESTS}")
