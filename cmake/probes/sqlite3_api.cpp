// CMake configure-time probe: pins the exact libsqlite3 API surface
// aid_auth uses. Linked against AID_SQLITE3_LIB during configure; not
// part of the daemon.
//
// Floor: SQLite 3.40 (Debian bookworm). SQLITE_OPEN_NOFOLLOW is the
// reason for the floor — it landed in 3.31. If this file refuses to
// link or compile, libsqlite3-dev is missing or too old — the
// FATAL_ERROR in the top-level CMakeLists shows the apt incantation
// and the probe build output.

#include <sqlite3.h>

#include <cstring>

int main() {
    // Constants must exist as compile-time integers.
    constexpr int flags =
        SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    (void)flags;

    sqlite3* db = nullptr;
    if (sqlite3_open_v2(":memory:", &db, flags, nullptr) != SQLITE_OK) {
        sqlite3_close_v2(db);
        return 1;
    }

    if (sqlite3_exec(db, "CREATE TABLE t(x);", nullptr, nullptr, nullptr) != SQLITE_OK) {
        sqlite3_close_v2(db);
        return 2;
    }

    sqlite3_stmt* stmt = nullptr;
    const char* sql = "SELECT 1";
    if (sqlite3_prepare_v2(db, sql, static_cast<int>(std::strlen(sql)), &stmt, nullptr) !=
        SQLITE_OK) {
        sqlite3_close_v2(db);
        return 3;
    }
    if (sqlite3_step(stmt) != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        sqlite3_close_v2(db);
        return 4;
    }
    sqlite3_finalize(stmt);
    sqlite3_close_v2(db);
    return 0;
}
