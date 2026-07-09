#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <unordered_map>

#include "aid/plumbing/Result.h"

// Forward declarations from <sqlite3.h>. Keeping the SQLite header out
// of our own public surface means consumers (slice 2 repos and beyond)
// pay for it only in their own .cpp files.
struct sqlite3;
struct sqlite3_stmt;

namespace aid::auth {

// AuthDb owns the single SQLite connection for the auth database.
// Opens the file, enforces the mode invariant (refuses any file
// wider than 0600), applies forward-only migrations via PRAGMA
// user_version, and exposes a prepared-statement cache.
//
// Concurrency: pinned to a single Drogon loop (the domain loop)
// at runtime. SQLITE_OPEN_FULLMUTEX is set defensively. Tests on the
// other hand drive this from the main thread directly.
class AuthDb {
public:
    AuthDb(const AuthDb&) = delete;
    AuthDb& operator=(const AuthDb&) = delete;
    AuthDb(AuthDb&& other) noexcept;
    AuthDb& operator=(AuthDb&& other) noexcept;
    ~AuthDb();

    // Opens the database at `path`. On first creation the file is left
    // at mode 0600; on subsequent opens the mode is checked and the
    // call fails if it's wider than 0600 or the path is a symlink.
    // Sets WAL + foreign_keys + busy_timeout, runs migrate().
    [[nodiscard]] static aid::plumbing::Result<AuthDb> open(std::filesystem::path path);

    // RAII guard that resets a prepared statement (sqlite3_reset +
    // sqlite3_clear_bindings) on scope exit. Callers construct one right
    // after prepare() so the statement's implicit read transaction is
    // released as soon as the query finishes. This matters because the
    // daemon holds a single long-lived connection: a stepped-but-unreset
    // SELECT pins the connection's WAL read snapshot, hiding committed
    // writes from other connections (e.g. aid-admin) until the process
    // restarts. Releasing the read txn returns the connection to
    // autocommit so the next query sees a fresh snapshot. The destructor
    // is defined out-of-line in AuthDb.cpp to keep <sqlite3.h> out of
    // this public header.
    class StmtScope {
    public:
        explicit StmtScope(sqlite3_stmt* stmt) noexcept : stmt_(stmt) {}
        ~StmtScope();
        StmtScope(const StmtScope&) = delete;
        StmtScope& operator=(const StmtScope&) = delete;
        StmtScope(StmtScope&&) = delete;
        StmtScope& operator=(StmtScope&&) = delete;

    private:
        sqlite3_stmt* stmt_;
    };

    // Cache-or-compile a prepared statement keyed by SQL text. The
    // returned pointer is owned by *this and remains valid until
    // ~AuthDb. Returns nullptr if compilation failed (callers in
    // slice 2 will branch on this).
    [[nodiscard]] sqlite3_stmt* prepare(std::string_view sql);

    // One-shot sqlite3_exec — schema and PRAGMA only. Returns the
    // total rows-changed count.
    [[nodiscard]] aid::plumbing::Result<int> exec(std::string_view sql);

    [[nodiscard]] sqlite3* handle() noexcept { return db_; }
    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    AuthDb() = default;
    [[nodiscard]] aid::plumbing::Result<void> migrate();
    void closeQuietly() noexcept;

    sqlite3* db_ = nullptr;
    std::filesystem::path path_;
    std::unordered_map<std::string, sqlite3_stmt*> stmtCache_;
};

} // namespace aid::auth
