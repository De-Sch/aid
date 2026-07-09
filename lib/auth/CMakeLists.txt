add_library(aid_auth STATIC
    AuthDb.cpp
    PasswordHasher.cpp
    UserRepo.cpp
    SessionRepo.cpp
    AuthService.cpp
    ResetGrantStore.cpp
    UserGate.cpp
)

target_include_directories(aid_auth
    PUBLIC ${CMAKE_SOURCE_DIR}/include
)

# sqlite3 / sodium are PRIVATE: AuthDb.h forward-declares sqlite3 +
# sqlite3_stmt and PasswordHasher.h doesn't include <sodium.h> at all,
# so consumers of aid_auth never see these headers transitively.
#
# aid_crosscutting is PUBLIC because UserRepo.h and SessionRepo.h
# reference Clock& and const AuthConfig& in their member declarations;
# consumers must see those types via Config.h / Clock.h.
target_link_libraries(aid_auth
    PUBLIC  aid_plumbing aid_value_types aid_crosscutting
    PRIVATE aid_warnings aid_sanitizers sqlite3 sodium
)
