#include <gtest/gtest.h>
#include <sqlite3.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "aid/auth/AuthDb.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::plumbing::ErrorCode;

namespace {

// Per-test scratch directory under /tmp. Mirrors the pattern used by
// tests/crosscutting/test_config.cpp so behaviour is consistent across
// the suite.
struct ScratchDir {
    fs::path dir;
    fs::path dbPath;

    ~ScratchDir() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

ScratchDir makeScratch() {
    static std::atomic<std::uint64_t> counter{0};
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    std::ostringstream name;
    name << "aid_authdb_" << pid << "_" << now << "_" << seq;

    ScratchDir s;
    s.dir = fs::temp_directory_path() / name.str();
    fs::create_directories(s.dir);
    s.dbPath = s.dir / "auth.db";
    return s;
}

// Returns the lower 7-bit POSIX mode bits of a file (octal).
::mode_t fileMode(const fs::path& p) {
    struct ::stat st {};
    if (::lstat(p.c_str(), &st) != 0) {
        return 0;
    }
    return st.st_mode & 07777;
}

// One-shot scalar SELECT against the db's raw handle. Returns -1 on any
// SQLite-side problem; tests check for the specific expected value.
int pragmaInt(sqlite3* db, const char* pragma) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, pragma, -1, &stmt, nullptr) != SQLITE_OK) {
        return -1;
    }
    int v = -1;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        v = sqlite3_column_int(stmt, 0);
    }
    sqlite3_finalize(stmt);
    return v;
}

std::string pragmaText(sqlite3* db, const char* pragma) {
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, pragma, -1, &stmt, nullptr) != SQLITE_OK) {
        return {};
    }
    std::string out;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p != nullptr) {
            out = p;
        }
    }
    sqlite3_finalize(stmt);
    return out;
}

std::vector<std::string> sqliteMasterNames(sqlite3* db) {
    std::vector<std::string> names;
    sqlite3_stmt* stmt = nullptr;
    if (sqlite3_prepare_v2(db, "SELECT name FROM sqlite_master ORDER BY name;", -1, &stmt,
                           nullptr) != SQLITE_OK) {
        return names;
    }
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const auto* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
        if (p != nullptr) {
            names.emplace_back(p);
        }
    }
    sqlite3_finalize(stmt);
    return names;
}

bool contains(const std::vector<std::string>& v, const std::string& s) {
    for (const auto& x : v) {
        if (x == s) {
            return true;
        }
    }
    return false;
}

} // namespace

TEST(AuthDb, OpenCreatesFreshDbInWalMode) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    EXPECT_TRUE(fs::exists(s.dbPath));
    EXPECT_EQ(fileMode(s.dbPath), ::mode_t{0600});

    EXPECT_EQ(pragmaText(r->handle(), "PRAGMA journal_mode;"), "wal");
    EXPECT_EQ(pragmaInt(r->handle(), "PRAGMA foreign_keys;"), 1);
    EXPECT_EQ(pragmaInt(r->handle(), "PRAGMA user_version;"), 1);
}

TEST(AuthDb, OpenAppliesSchemaOnFirstRun) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    const auto names = sqliteMasterNames(r->handle());
    EXPECT_TRUE(contains(names, "users")) << "users table missing";
    EXPECT_TRUE(contains(names, "sessions")) << "sessions table missing";
    EXPECT_TRUE(contains(names, "idx_sessions_prefix")) << "prefix index missing";
    EXPECT_TRUE(contains(names, "idx_sessions_user")) << "user index missing";
}

TEST(AuthDb, OpenIsIdempotent) {
    auto s = makeScratch();
    {
        auto r1 = AuthDb::open(s.dbPath);
        ASSERT_TRUE(r1.has_value()) << r1.error().message;
    } // dropped → close
    auto r2 = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r2.has_value()) << r2.error().message;
    EXPECT_EQ(pragmaInt(r2->handle(), "PRAGMA user_version;"), 1);
}

TEST(AuthDb, RefusesGroupReadableFile) {
    for (::mode_t mode : {::mode_t{0640}, ::mode_t{0644}, ::mode_t{0660}, ::mode_t{0666}}) {
        auto s = makeScratch();
        { std::ofstream{s.dbPath} << ""; }
        ASSERT_EQ(::chmod(s.dbPath.c_str(), mode), 0);

        auto r = AuthDb::open(s.dbPath);
        ASSERT_FALSE(r.has_value()) << "mode " << std::oct << mode << " should be refused";
        EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
        EXPECT_NE(r.error().message.find("0600"), std::string::npos)
            << "error should mention the policy: " << r.error().message;
    }
}

TEST(AuthDb, RefusesSymlink) {
    auto s = makeScratch();
    const auto target = s.dir / "real-auth.db";
    { std::ofstream{target} << ""; }
    ASSERT_EQ(::chmod(target.c_str(), 0600), 0);
    ASSERT_EQ(::symlink(target.c_str(), s.dbPath.c_str()), 0);

    auto r = AuthDb::open(s.dbPath);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("symlink"), std::string::npos) << r.error().message;
}

TEST(AuthDb, RefusesUnknownUserVersion) {
    auto s = makeScratch();
    // Seed a fresh db at user_version = 99 by going around AuthDb.
    sqlite3* raw = nullptr;
    ASSERT_EQ(sqlite3_open_v2(s.dbPath.c_str(), &raw,
                              SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX,
                              nullptr),
              SQLITE_OK);
    ASSERT_EQ(sqlite3_exec(raw, "PRAGMA user_version = 99;", nullptr, nullptr, nullptr), SQLITE_OK);
    sqlite3_close_v2(raw);
    ASSERT_EQ(::chmod(s.dbPath.c_str(), 0600), 0);

    auto r = AuthDb::open(s.dbPath);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvariantViolation);
    EXPECT_NE(r.error().message.find("user_version"), std::string::npos) << r.error().message;
}

TEST(AuthDb, PrepareCachesByText) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto* a = r->prepare("SELECT 1");
    auto* b = r->prepare("SELECT 1");
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(a, b) << "second prepare of same SQL text should hit the cache";

    auto* c = r->prepare("SELECT 2");
    ASSERT_NE(c, nullptr);
    EXPECT_NE(a, c) << "different SQL text should compile a fresh statement";
}

TEST(AuthDb, PrepareReturnsNullForBadSql) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->prepare("THIS IS NOT SQL"), nullptr);
}

TEST(AuthDb, ExecRunsSingleStatement) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;

    auto first = r->exec("CREATE TABLE t(x INTEGER);");
    EXPECT_TRUE(first.has_value()) << first.error().message;

    // Second CREATE on the same name surfaces a SQLite-side failure.
    auto second = r->exec("CREATE TABLE t(x INTEGER);");
    EXPECT_FALSE(second.has_value());
}

TEST(AuthDb, DestructorFinalizesCachedStatements) {
    auto s = makeScratch();
    {
        auto r = AuthDb::open(s.dbPath);
        ASSERT_TRUE(r.has_value()) << r.error().message;
        EXPECT_NE(r->prepare("SELECT 1"), nullptr);
        EXPECT_NE(r->prepare("SELECT 2"), nullptr);
        EXPECT_NE(r->prepare("SELECT 3"), nullptr);
        // Drop scope — destructor must finalise everything and close the
        // handle cleanly. ASan + UBSan verify no leak / use-after.
    }
    // If we got here without aborting, the destructor path is sound.
    SUCCEED();
}

TEST(AuthDb, MoveLeavesSourceDestructible) {
    auto s = makeScratch();
    auto r = AuthDb::open(s.dbPath);
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_NE(r->prepare("SELECT 1"), nullptr);

    AuthDb moved{std::move(*r)};
    // Source's destructor must be a no-op now (no double-finalize, no
    // double-close). ASan would flag otherwise.
    EXPECT_EQ(pragmaInt(moved.handle(), "PRAGMA user_version;"), 1);
}
