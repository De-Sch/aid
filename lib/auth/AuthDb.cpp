#include "aid/auth/AuthDb.h"

#include <sqlite3.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>
#include <utility>

#include "aid/plumbing/Error.h"

namespace aid::auth {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

namespace {

Error makeError(ErrorCode code, std::string msg) {
    return Error{code, std::move(msg), std::nullopt};
}

Error sqliteError(sqlite3* db, std::string_view ctx) {
    std::ostringstream m;
    m << "auth.db: " << ctx;
    if (db != nullptr) {
        m << ": " << sqlite3_errmsg(db);
    }
    return makeError(ErrorCode::Unknown, m.str());
}

// Returns the current PRAGMA user_version (0 on a fresh database).
// Returns -1 on SQLite-side failure, which the caller maps to an Error.
[[nodiscard]] int readUserVersion(sqlite3* db) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    int version = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        version = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return version;
}

// Raw exec wrapper — returns SQLite's rc; caller wraps into Error.
[[nodiscard]] int rawExec(sqlite3* db, const char* sql) {
    char* err = nullptr;
    const int rc = sqlite3_exec(db, sql, nullptr, nullptr, &err);
    if (err != nullptr) {
        sqlite3_free(err);
    }
    return rc;
}

// The daemon refuses to start if auth.db is group/world-readable.
// We stat with lstat first so a symlink doesn't squeeze us through —
// SQLITE_OPEN_NOFOLLOW is belt-and-braces on top of this.
[[nodiscard]] Result<void> checkExistingFileMode(const std::filesystem::path& p) {
    struct ::stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return unexpected(makeError(ErrorCode::Unknown,
                                    std::string{"auth.db: lstat failed: "} + std::strerror(errno)));
    }
    if (S_ISLNK(st.st_mode)) {
        return unexpected(makeError(ErrorCode::InvalidInput,
                                    "auth.db: refusing to open a symlink at " + p.string()));
    }
    if (!S_ISREG(st.st_mode)) {
        return unexpected(
            makeError(ErrorCode::InvalidInput, "auth.db: not a regular file: " + p.string()));
    }
    if ((st.st_mode & 0177) != 0) {
        std::ostringstream m;
        m << "auth.db: file mode wider than 0600 (got 0" << std::oct << (st.st_mode & 07777)
          << std::dec << ") at " << p.string();
        return unexpected(makeError(ErrorCode::InvalidInput, m.str()));
    }
    const ::uid_t self = ::geteuid();
    if (st.st_uid != self && st.st_uid != 0) {
        std::ostringstream m;
        m << "auth.db: owner uid " << st.st_uid << " is neither effective uid " << self
          << " nor root at " << p.string();
        return unexpected(makeError(ErrorCode::InvalidInput, m.str()));
    }
    return {};
}

// RAII umask guard: forces a restrictive mask while SQLite creates the
// file, restores the caller's mask on scope exit.
class UmaskGuard {
public:
    explicit UmaskGuard(::mode_t mask) noexcept : prev_(::umask(mask)) {}
    UmaskGuard(const UmaskGuard&) = delete;
    UmaskGuard& operator=(const UmaskGuard&) = delete;
    UmaskGuard(UmaskGuard&&) = delete;
    UmaskGuard& operator=(UmaskGuard&&) = delete;
    ~UmaskGuard() { ::umask(prev_); }

private:
    ::mode_t prev_;
};

// v1 schema.
constexpr const char* kSchemaV1 = R"sql(
CREATE TABLE users (
    id                INTEGER PRIMARY KEY,
    username          TEXT NOT NULL UNIQUE,
    password_hash     TEXT NOT NULL,
    failed_attempts   INTEGER NOT NULL DEFAULT 0,
    locked_until      INTEGER,
    created_at        INTEGER NOT NULL,
    last_login_at     INTEGER
);

CREATE TABLE sessions (
    id            INTEGER PRIMARY KEY,
    user_id       INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
    token_hash    TEXT NOT NULL UNIQUE,
    prefix        TEXT NOT NULL,
    created_at    INTEGER NOT NULL,
    expires_at    INTEGER NOT NULL,
    last_seen_at  INTEGER NOT NULL,
    ip_at_login   TEXT,
    user_agent    TEXT
);
CREATE INDEX idx_sessions_prefix ON sessions(prefix);
CREATE INDEX idx_sessions_user   ON sessions(user_id);
)sql";

} // namespace

AuthDb::AuthDb(AuthDb&& other) noexcept
    : db_(other.db_), path_(std::move(other.path_)), stmtCache_(std::move(other.stmtCache_)) {
    other.db_ = nullptr;
    other.stmtCache_.clear();
}

AuthDb& AuthDb::operator=(AuthDb&& other) noexcept {
    if (this != &other) {
        closeQuietly();
        db_ = other.db_;
        path_ = std::move(other.path_);
        stmtCache_ = std::move(other.stmtCache_);
        other.db_ = nullptr;
        other.stmtCache_.clear();
    }
    return *this;
}

AuthDb::~AuthDb() {
    closeQuietly();
}

void AuthDb::closeQuietly() noexcept {
    for (auto& kv : stmtCache_) {
        if (kv.second != nullptr) {
            sqlite3_finalize(kv.second);
        }
    }
    stmtCache_.clear();
    if (db_ != nullptr) {
        sqlite3_close_v2(db_);
        db_ = nullptr;
    }
}

Result<AuthDb> AuthDb::open(std::filesystem::path path) {
    namespace fs = std::filesystem;

    const bool exists = fs::exists(path);
    if (exists) {
        auto modeCheck = checkExistingFileMode(path);
        if (!modeCheck) {
            return unexpected(modeCheck.error());
        }
    } else {
        // Make sure the parent directory exists; SQLITE_OPEN_CREATE
        // will not mkdir it for us.
        const auto parent = path.parent_path();
        if (!parent.empty() && !fs::exists(parent)) {
            std::ostringstream m;
            m << "auth.db: parent directory does not exist: " << parent.string();
            return unexpected(makeError(ErrorCode::InvalidInput, m.str()));
        }
    }

    int flags = SQLITE_OPEN_READWRITE | SQLITE_OPEN_FULLMUTEX | SQLITE_OPEN_NOFOLLOW;
    if (!exists) {
        flags |= SQLITE_OPEN_CREATE;
    }

    sqlite3* raw = nullptr;
    int rc = SQLITE_OK;
    {
        // 0177 mask leaves 0600 on the created file.
        UmaskGuard guard{exists ? ::mode_t{0} : ::mode_t{0177}};
        rc = sqlite3_open_v2(path.c_str(), &raw, flags, nullptr);
    }
    if (rc != SQLITE_OK) {
        Error err = sqliteError(raw, "sqlite3_open_v2 failed for " + path.string());
        if (raw != nullptr) {
            sqlite3_close_v2(raw);
        }
        return unexpected(std::move(err));
    }

    // Belt: chmod on the file we just created. If we lost the race
    // (umask interaction with a process-wide umask change from another
    // thread), this still narrows the window.
    if (!exists) {
        if (::chmod(path.c_str(), 0600) != 0) {
            sqlite3_close_v2(raw);
            return unexpected(
                makeError(ErrorCode::Unknown,
                          std::string{"auth.db: chmod 0600 failed: "} + std::strerror(errno)));
        }
    }

    AuthDb out;
    out.db_ = raw;
    out.path_ = std::move(path);

    // Pragmas before migrate so they take effect for the schema apply
    // path too. journal_mode=WAL persists across opens; the others are
    // per-connection.
    if (rawExec(out.db_, "PRAGMA journal_mode = WAL;") != SQLITE_OK) {
        return unexpected(sqliteError(out.db_, "PRAGMA journal_mode = WAL"));
    }
    if (rawExec(out.db_, "PRAGMA foreign_keys = ON;") != SQLITE_OK) {
        return unexpected(sqliteError(out.db_, "PRAGMA foreign_keys = ON"));
    }
    if (rawExec(out.db_, "PRAGMA busy_timeout = 5000;") != SQLITE_OK) {
        return unexpected(sqliteError(out.db_, "PRAGMA busy_timeout = 5000"));
    }

    auto m = out.migrate();
    if (!m) {
        return unexpected(m.error());
    }
    return out;
}

Result<void> AuthDb::migrate() {
    const int version = readUserVersion(db_);
    if (version < 0) {
        return unexpected(sqliteError(db_, "reading PRAGMA user_version"));
    }
    if (version == 1) {
        return {};
    }
    if (version != 0) {
        std::ostringstream m;
        m << "auth.db: unknown user_version: " << version;
        return unexpected(makeError(ErrorCode::InvariantViolation, m.str()));
    }

    if (rawExec(db_, "BEGIN IMMEDIATE;") != SQLITE_OK) {
        return unexpected(sqliteError(db_, "begin migration tx"));
    }
    if (rawExec(db_, kSchemaV1) != SQLITE_OK) {
        Error err = sqliteError(db_, "applying v1 schema");
        // best-effort rollback; original error is what we report.
        (void)rawExec(db_, "ROLLBACK;");
        return unexpected(std::move(err));
    }
    if (rawExec(db_, "PRAGMA user_version = 1;") != SQLITE_OK) {
        Error err = sqliteError(db_, "setting user_version = 1");
        (void)rawExec(db_, "ROLLBACK;");
        return unexpected(std::move(err));
    }
    if (rawExec(db_, "COMMIT;") != SQLITE_OK) {
        return unexpected(sqliteError(db_, "committing migration tx"));
    }
    return {};
}

AuthDb::StmtScope::~StmtScope() {
    if (stmt_ != nullptr) {
        sqlite3_reset(stmt_);
        sqlite3_clear_bindings(stmt_);
    }
}

sqlite3_stmt* AuthDb::prepare(std::string_view sql) {
    if (db_ == nullptr) {
        return nullptr;
    }
    std::string key{sql};
    if (auto it = stmtCache_.find(key); it != stmtCache_.end()) {
        sqlite3_reset(it->second);
        sqlite3_clear_bindings(it->second);
        return it->second;
    }
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db_, key.c_str(), static_cast<int>(key.size()), &stmt, nullptr) !=
        SQLITE_OK) {
        if (stmt != nullptr) {
            sqlite3_finalize(stmt);
        }
        return nullptr;
    }
    stmtCache_.emplace(std::move(key), stmt);
    return stmt;
}

Result<int> AuthDb::exec(std::string_view sql) {
    if (db_ == nullptr) {
        return unexpected(
            makeError(ErrorCode::InvariantViolation, "auth.db: exec on closed handle"));
    }
    std::string copy{sql};
    if (rawExec(db_, copy.c_str()) != SQLITE_OK) {
        return unexpected(sqliteError(db_, "exec: " + copy));
    }
    return sqlite3_changes(db_);
}

} // namespace aid::auth
