#include "aid/auth/UserRepo.h"

#include <sqlite3.h>

#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

#include "aid/auth/AuthDb.h"
#include "aid/crosscutting/Clock.h"
#include "aid/plumbing/Error.h"

namespace aid::auth {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::unexpected;

namespace {

[[nodiscard]] Error makeError(ErrorCode code, std::string msg) {
    return Error{code, std::move(msg), std::nullopt};
}

[[nodiscard]] Error sqliteError(sqlite3* db, std::string_view ctx) {
    std::ostringstream m;
    m << "auth.db: " << ctx;
    if (db != nullptr) {
        m << ": " << sqlite3_errmsg(db);
    }
    return makeError(ErrorCode::Unknown, m.str());
}

// Unix epoch seconds <-> Timestamp. SQLite stores INTEGER timestamps.
[[nodiscard]] std::int64_t toEpochSeconds(aid::Timestamp t) {
    return std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

[[nodiscard]] aid::Timestamp fromEpochSeconds(std::int64_t s) {
    return aid::Timestamp{std::chrono::seconds{s}};
}

[[nodiscard]] std::string_view columnText(sqlite3_stmt* stmt, int col) {
    const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, col));
    if (p == nullptr) {
        return {};
    }
    const auto n = sqlite3_column_bytes(stmt, col);
    return std::string_view{p, static_cast<std::size_t>(n)};
}

[[nodiscard]] UserRepo::User readUserRow(sqlite3_stmt* stmt) {
    UserRepo::User u{};
    u.id = sqlite3_column_int64(stmt, 0);
    u.handle = aid::UserHandle{std::string{columnText(stmt, 1)}};
    u.passwordHash = std::string{columnText(stmt, 2)};
    u.createdAt = fromEpochSeconds(sqlite3_column_int64(stmt, 3));
    if (sqlite3_column_type(stmt, 4) != SQLITE_NULL) {
        u.lastLoginAt = fromEpochSeconds(sqlite3_column_int64(stmt, 4));
    }
    return u;
}

constexpr const char* kSelectByUsername =
    "SELECT id, username, password_hash, created_at, last_login_at "
    "FROM users WHERE username = ?;";

constexpr const char* kSelectById = "SELECT id, username, password_hash, created_at, last_login_at "
                                    "FROM users WHERE id = ?;";

// The v1 `users` schema also has `failed_attempts NOT NULL DEFAULT 0`
// and a nullable `locked_until` — both vestigial after L7. We let the
// DEFAULT cover failed_attempts here so this INSERT doesn't have to
// know about a column it doesn't conceptually care about; the v2
// migration that drops both columns can then change the schema
// without touching this string.
constexpr const char* kInsertUser = "INSERT INTO users (username, password_hash, created_at) "
                                    "VALUES (?, ?, ?);";

constexpr const char* kDeleteUser = "DELETE FROM users WHERE id = ?;";

constexpr const char* kSetPasswordHash = "UPDATE users SET password_hash = ? WHERE id = ?;";

constexpr const char* kRecordSuccess = "UPDATE users SET last_login_at = ? WHERE id = ?;";

constexpr const char* kListAllUsers =
    "SELECT id, username, password_hash, created_at, last_login_at "
    "FROM users ORDER BY id ASC;";

} // namespace

UserRepo::UserRepo(AuthDb& db, aid::crosscutting::Clock& clock) noexcept : db_(db), clock_(clock) {
}

Result<std::optional<UserRepo::User>> UserRepo::lookupByUsername(std::string_view name) const {
    sqlite3_stmt* stmt = db_.prepare(kSelectByUsername);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare lookupByUsername"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_text(stmt, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT) !=
        SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind lookupByUsername"));
    }
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return std::optional<User>{};
    }
    if (rc != SQLITE_ROW) {
        return unexpected(sqliteError(db_.handle(), "step lookupByUsername"));
    }
    // UNIQUE index on users.username guarantees one row. readUserRow
    // deep-copies the columns into owned strings before `scope` resets
    // the statement on return; the reset releases the implicit read
    // transaction so the connection's WAL snapshot advances and a later
    // lookup sees committed writes from other connections (e.g.
    // aid-admin) without a daemon restart.
    return std::optional<User>{readUserRow(stmt)};
}

Result<std::optional<UserRepo::User>> UserRepo::lookupById(std::int64_t id) const {
    sqlite3_stmt* stmt = db_.prepare(kSelectById);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare lookupById"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind lookupById"));
    }
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return std::optional<User>{};
    }
    if (rc != SQLITE_ROW) {
        return unexpected(sqliteError(db_.handle(), "step lookupById"));
    }
    return std::optional<User>{readUserRow(stmt)};
}

Result<std::int64_t> UserRepo::create(std::string_view name, std::string_view passwordHash) {
    if (name.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "username must not be empty"));
    }
    if (passwordHash.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "password_hash must not be empty"));
    }
    sqlite3_stmt* stmt = db_.prepare(kInsertUser);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare create"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_text(stmt, 1, name.data(), static_cast<int>(name.size()), SQLITE_TRANSIENT) !=
            SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, passwordHash.data(), static_cast<int>(passwordHash.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, toEpochSeconds(clock_.now())) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind create"));
    }
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        // SQLITE_CONSTRAINT covers UNIQUE on username, NOT NULL, etc.
        // Map the UNIQUE case to the Conflict code; other constraint
        // failures fall through to Unknown with the sqlite error text.
        const int ext = sqlite3_extended_errcode(db_.handle());
        if (rc == SQLITE_CONSTRAINT && ext == SQLITE_CONSTRAINT_UNIQUE) {
            return unexpected(makeError(ErrorCode::Conflict, "username already exists"));
        }
        return unexpected(sqliteError(db_.handle(), "step create"));
    }
    return sqlite3_last_insert_rowid(db_.handle());
}

Result<void> UserRepo::deleteUser(std::int64_t id) {
    sqlite3_stmt* stmt = db_.prepare(kDeleteUser);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare deleteUser"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, id) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind deleteUser"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step deleteUser"));
    }
    return {};
}

Result<void> UserRepo::setPasswordHash(std::int64_t id, std::string_view newHash) {
    if (newHash.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "new hash must not be empty"));
    }
    sqlite3_stmt* stmt = db_.prepare(kSetPasswordHash);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare setPasswordHash"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_text(stmt, 1, newHash.data(), static_cast<int>(newHash.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, id) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind setPasswordHash"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step setPasswordHash"));
    }
    if (sqlite3_changes(db_.handle()) == 0) {
        return unexpected(makeError(ErrorCode::NotFound, "user id not found"));
    }
    return {};
}

Result<void> UserRepo::recordSuccessfulLogin(std::int64_t id) {
    sqlite3_stmt* stmt = db_.prepare(kRecordSuccess);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare recordSuccessfulLogin"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, toEpochSeconds(clock_.now())) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, id) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind recordSuccessfulLogin"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step recordSuccessfulLogin"));
    }
    if (sqlite3_changes(db_.handle()) == 0) {
        return unexpected(makeError(ErrorCode::NotFound, "user id not found"));
    }
    return {};
}

Result<std::vector<UserRepo::User>> UserRepo::listAll() const {
    sqlite3_stmt* stmt = db_.prepare(kListAllUsers);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare listAll"));
    }
    AuthDb::StmtScope scope{stmt};
    std::vector<User> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return unexpected(sqliteError(db_.handle(), "step listAll"));
        }
        out.push_back(readUserRow(stmt));
    }
    return out;
}

} // namespace aid::auth
