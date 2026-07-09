#include "aid/auth/SessionRepo.h"

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
#include "aid/crosscutting/Config.h"
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

[[nodiscard]] SessionRepo::Session readSessionRow(sqlite3_stmt* stmt) {
    SessionRepo::Session s{};
    s.id = sqlite3_column_int64(stmt, 0);
    s.userId = sqlite3_column_int64(stmt, 1);
    s.tokenHash = std::string{columnText(stmt, 2)};
    s.prefix = std::string{columnText(stmt, 3)};
    s.createdAt = fromEpochSeconds(sqlite3_column_int64(stmt, 4));
    s.expiresAt = fromEpochSeconds(sqlite3_column_int64(stmt, 5));
    s.lastSeenAt = fromEpochSeconds(sqlite3_column_int64(stmt, 6));
    if (sqlite3_column_type(stmt, 7) != SQLITE_NULL) {
        s.ipAtLogin = std::string{columnText(stmt, 7)};
    }
    if (sqlite3_column_type(stmt, 8) != SQLITE_NULL) {
        s.userAgent = std::string{columnText(stmt, 8)};
    }
    return s;
}

constexpr const char* kSelectByTokenHash =
    "SELECT id, user_id, token_hash, prefix, created_at, expires_at, last_seen_at, ip_at_login, "
    "user_agent "
    "FROM sessions WHERE token_hash = ?;";

constexpr const char* kInsertSession =
    "INSERT INTO sessions (user_id, token_hash, prefix, created_at, expires_at, last_seen_at, "
    "ip_at_login, user_agent) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?);";

constexpr const char* kSlide = "UPDATE sessions SET expires_at = ?, last_seen_at = ? WHERE id = ?;";

constexpr const char* kRevokeByHash = "DELETE FROM sessions WHERE token_hash = ?;";

constexpr const char* kRevokeAllForUser = "DELETE FROM sessions WHERE user_id = ?;";

constexpr const char* kPrune = "DELETE FROM sessions WHERE expires_at <= ?;";

constexpr const char* kListAll =
    "SELECT id, user_id, token_hash, prefix, created_at, expires_at, last_seen_at, ip_at_login, "
    "user_agent FROM sessions ORDER BY id ASC;";

constexpr const char* kListAllForUser =
    "SELECT id, user_id, token_hash, prefix, created_at, expires_at, last_seen_at, ip_at_login, "
    "user_agent FROM sessions WHERE user_id = ? ORDER BY id ASC;";

// Binds a string-or-NULL: when the view is empty we bind SQL NULL,
// matching the schema's nullable ip_at_login / user_agent columns.
[[nodiscard]] int bindOptionalText(sqlite3_stmt* stmt, int idx, std::string_view v) {
    if (v.empty()) {
        return sqlite3_bind_null(stmt, idx);
    }
    return sqlite3_bind_text(stmt, idx, v.data(), static_cast<int>(v.size()), SQLITE_TRANSIENT);
}

} // namespace

SessionRepo::SessionRepo(AuthDb& db, aid::crosscutting::Clock& clock,
                         const aid::crosscutting::AuthConfig& cfg) noexcept
    : db_(db), clock_(clock), cfg_(cfg) {
}

Result<SessionRepo::Session> SessionRepo::create(std::int64_t userId, std::string_view tokenHash,
                                                 std::string_view prefix,
                                                 std::string_view ipAtLogin,
                                                 std::string_view userAgent) {
    if (tokenHash.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "tokenHash must not be empty"));
    }
    if (prefix.empty()) {
        return unexpected(makeError(ErrorCode::InvalidInput, "prefix must not be empty"));
    }

    const auto now = clock_.now();
    const auto expiresAt = now + std::chrono::seconds{cfg_.sessionLifetimeSeconds};

    sqlite3_stmt* stmt = db_.prepare(kInsertSession);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare create session"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, userId) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 2, tokenHash.data(), static_cast<int>(tokenHash.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_text(stmt, 3, prefix.data(), static_cast<int>(prefix.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 4, toEpochSeconds(now)) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 5, toEpochSeconds(expiresAt)) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 6, toEpochSeconds(now)) != SQLITE_OK ||
        bindOptionalText(stmt, 7, ipAtLogin) != SQLITE_OK ||
        bindOptionalText(stmt, 8, userAgent) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind create session"));
    }
    const int rc = sqlite3_step(stmt);
    if (rc != SQLITE_DONE) {
        const int ext = sqlite3_extended_errcode(db_.handle());
        if (rc == SQLITE_CONSTRAINT && ext == SQLITE_CONSTRAINT_UNIQUE) {
            return unexpected(makeError(ErrorCode::Conflict, "session token already exists"));
        }
        return unexpected(sqliteError(db_.handle(), "step create session"));
    }
    Session s{};
    s.id = sqlite3_last_insert_rowid(db_.handle());
    s.userId = userId;
    s.tokenHash = std::string{tokenHash};
    s.prefix = std::string{prefix};
    s.createdAt = now;
    s.expiresAt = expiresAt;
    s.lastSeenAt = now;
    if (!ipAtLogin.empty()) {
        s.ipAtLogin = std::string{ipAtLogin};
    }
    if (!userAgent.empty()) {
        s.userAgent = std::string{userAgent};
    }
    return s;
}

Result<std::optional<SessionRepo::Session>>
SessionRepo::lookupByTokenHash(std::string_view tokenHash) const {
    sqlite3_stmt* stmt = db_.prepare(kSelectByTokenHash);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare lookupByTokenHash"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_text(stmt, 1, tokenHash.data(), static_cast<int>(tokenHash.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind lookupByTokenHash"));
    }
    const int rc = sqlite3_step(stmt);
    if (rc == SQLITE_DONE) {
        return std::optional<Session>{};
    }
    if (rc != SQLITE_ROW) {
        return unexpected(sqliteError(db_.handle(), "step lookupByTokenHash"));
    }
    // token_hash carries UNIQUE so at most one row matches.
    return std::optional<Session>{readSessionRow(stmt)};
}

Result<void> SessionRepo::slide(std::int64_t sessionId) {
    const auto now = clock_.now();
    const auto expiresAt = now + std::chrono::seconds{cfg_.sessionLifetimeSeconds};
    sqlite3_stmt* stmt = db_.prepare(kSlide);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare slide"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, toEpochSeconds(expiresAt)) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 2, toEpochSeconds(now)) != SQLITE_OK ||
        sqlite3_bind_int64(stmt, 3, sessionId) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind slide"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step slide"));
    }
    if (sqlite3_changes(db_.handle()) == 0) {
        return unexpected(makeError(ErrorCode::NotFound, "session id not found"));
    }
    return {};
}

Result<void> SessionRepo::revoke(std::string_view tokenHash) {
    sqlite3_stmt* stmt = db_.prepare(kRevokeByHash);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare revoke"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_text(stmt, 1, tokenHash.data(), static_cast<int>(tokenHash.size()),
                          SQLITE_TRANSIENT) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind revoke"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step revoke"));
    }
    return {};
}

Result<void> SessionRepo::revokeAllFor(std::int64_t userId) {
    sqlite3_stmt* stmt = db_.prepare(kRevokeAllForUser);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare revokeAllFor"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, userId) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind revokeAllFor"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step revokeAllFor"));
    }
    return {};
}

Result<int> SessionRepo::prune() {
    sqlite3_stmt* stmt = db_.prepare(kPrune);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare prune"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, toEpochSeconds(clock_.now())) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind prune"));
    }
    if (sqlite3_step(stmt) != SQLITE_DONE) {
        return unexpected(sqliteError(db_.handle(), "step prune"));
    }
    return sqlite3_changes(db_.handle());
}

Result<std::vector<SessionRepo::Session>> SessionRepo::listAll() const {
    sqlite3_stmt* stmt = db_.prepare(kListAll);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare listAll"));
    }
    AuthDb::StmtScope scope{stmt};
    std::vector<Session> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return unexpected(sqliteError(db_.handle(), "step listAll"));
        }
        out.push_back(readSessionRow(stmt));
    }
    return out;
}

Result<std::vector<SessionRepo::Session>> SessionRepo::listAllForUser(std::int64_t userId) const {
    sqlite3_stmt* stmt = db_.prepare(kListAllForUser);
    if (stmt == nullptr) {
        return unexpected(sqliteError(db_.handle(), "prepare listAllForUser"));
    }
    AuthDb::StmtScope scope{stmt};
    if (sqlite3_bind_int64(stmt, 1, userId) != SQLITE_OK) {
        return unexpected(sqliteError(db_.handle(), "bind listAllForUser"));
    }
    std::vector<Session> out;
    while (true) {
        const int rc = sqlite3_step(stmt);
        if (rc == SQLITE_DONE) {
            break;
        }
        if (rc != SQLITE_ROW) {
            return unexpected(sqliteError(db_.handle(), "step listAllForUser"));
        }
        out.push_back(readSessionRow(stmt));
    }
    return out;
}

} // namespace aid::auth
