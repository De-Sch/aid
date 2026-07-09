#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <sstream>
#include <string>

#include "FakeClock.h"
#include "aid/auth/AuthDb.h"
#include "aid/auth/UserRepo.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::UserRepo;
using aid::fakes::FakeClock;
using aid::plumbing::ErrorCode;

namespace {

struct Fixture {
    fs::path dir;
    fs::path dbPath;
    std::optional<AuthDb> db;
    FakeClock clock;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_userrepo_" << pid << "_" << now << "_" << seq;
        dir = fs::temp_directory_path() / name.str();
        fs::create_directories(dir);
        dbPath = dir / "auth.db";

        auto r = AuthDb::open(dbPath);
        if (!r) {
            // Tests will surface the message on first use; constructing
            // a fixture with a busted db is a hard failure.
            std::abort();
        }
        db.emplace(std::move(*r));
        clock.set(aid::Timestamp{std::chrono::seconds{1'700'000'000}});
    }

    ~Fixture() {
        db.reset();
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    UserRepo makeRepo() { return UserRepo{*db, clock}; }
};

} // namespace

TEST(UserRepo, LookupByUsernameReturnsNulloptForMissingUser) {
    Fixture f;
    auto repo = f.makeRepo();
    auto r = repo.lookupByUsername("nobody");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_FALSE(r->has_value());
}

TEST(UserRepo, CreateAndLookupRoundtrip) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("alice", "$argon2id$dummyhash");
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_GT(*id, 0);

    auto found = repo.lookupByUsername("alice");
    ASSERT_TRUE(found.has_value()) << found.error().message;
    ASSERT_TRUE(found->has_value()) << "fresh user should be found";
    EXPECT_EQ((*found)->id, *id);
    EXPECT_EQ((*found)->handle.v, "alice");
    EXPECT_EQ((*found)->passwordHash, "$argon2id$dummyhash");
    EXPECT_FALSE((*found)->lastLoginAt.has_value());
    EXPECT_EQ((*found)->createdAt, f.clock.now());
}

TEST(UserRepo, CreateRejectsDuplicateUsernameWithConflict) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create("alice", "h1").has_value());

    auto dup = repo.create("alice", "h2");
    ASSERT_FALSE(dup.has_value());
    EXPECT_EQ(dup.error().code, ErrorCode::Conflict);
    EXPECT_NE(dup.error().message.find("already exists"), std::string::npos);
}

TEST(UserRepo, CreateRejectsEmptyInputs) {
    Fixture f;
    auto repo = f.makeRepo();
    EXPECT_EQ(repo.create("", "h").error().code, ErrorCode::InvalidInput);
    EXPECT_EQ(repo.create("alice", "").error().code, ErrorCode::InvalidInput);
}

TEST(UserRepo, LookupByIdReturnsExistingUser) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("bob", "h");
    ASSERT_TRUE(id.has_value());

    auto by = repo.lookupById(*id);
    ASSERT_TRUE(by.has_value()) << by.error().message;
    ASSERT_TRUE(by->has_value());
    EXPECT_EQ((*by)->handle.v, "bob");
}

TEST(UserRepo, LookupByIdReturnsNulloptForMissingId) {
    Fixture f;
    auto repo = f.makeRepo();
    auto by = repo.lookupById(9999);
    ASSERT_TRUE(by.has_value()) << by.error().message;
    EXPECT_FALSE(by->has_value());
}

TEST(UserRepo, DeleteRemovesUser) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("alice", "h");
    ASSERT_TRUE(id.has_value());

    auto del = repo.deleteUser(*id);
    ASSERT_TRUE(del.has_value()) << del.error().message;

    auto by = repo.lookupById(*id);
    ASSERT_TRUE(by.has_value());
    EXPECT_FALSE(by->has_value());
}

TEST(UserRepo, SetPasswordHashUpdatesHash) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("alice", "old-hash");
    ASSERT_TRUE(id.has_value());

    auto setRes = repo.setPasswordHash(*id, "new-hash");
    ASSERT_TRUE(setRes.has_value()) << setRes.error().message;

    auto by = repo.lookupById(*id);
    ASSERT_TRUE(by.has_value());
    ASSERT_TRUE(by->has_value());
    EXPECT_EQ((*by)->passwordHash, "new-hash");
}

TEST(UserRepo, SetPasswordHashRejectsEmpty) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("alice", "h");
    ASSERT_TRUE(id.has_value());

    auto setRes = repo.setPasswordHash(*id, "");
    ASSERT_FALSE(setRes.has_value());
    EXPECT_EQ(setRes.error().code, ErrorCode::InvalidInput);
}

TEST(UserRepo, RecordSuccessfulLoginUpdatesLastLoginAt) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("alice", "h");
    ASSERT_TRUE(id.has_value());

    ASSERT_TRUE(repo.recordSuccessfulLogin(*id).has_value());
    auto by = repo.lookupById(*id);
    ASSERT_TRUE(by.has_value());
    ASSERT_TRUE(by->has_value());
    ASSERT_TRUE((*by)->lastLoginAt.has_value());
    EXPECT_EQ(*(*by)->lastLoginAt, f.clock.now());
}

TEST(UserRepo, RecordSuccessfulLoginOnMissingUserReturnsNotFound) {
    Fixture f;
    auto repo = f.makeRepo();
    auto r = repo.recordSuccessfulLogin(9999);
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST(UserRepo, SetPasswordHashOnMissingUserReturnsNotFound) {
    Fixture f;
    auto repo = f.makeRepo();
    auto r = repo.setPasswordHash(9999, "h");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

// Regression for the "must restart the daemon after an aid-admin change"
// bug. The daemon keeps one long-lived AuthDb connection; aid-admin writes
// on a SEPARATE connection. A SELECT that was stepped but left unreset pins
// the reader's WAL read snapshot, so the daemon kept returning stale rows
// until the process restarted. The AuthDb::StmtScope guard releases each
// statement's implicit read transaction on scope exit, so the long-lived
// reader must observe an external update on its very next query — no reopen.
TEST(UserRepo, ReaderSeesExternalPasswordUpdateWithoutReopen) {
    Fixture f; // f.db is the long-lived "daemon" reader connection.
    auto reader = f.makeRepo();

    // A second, independent connection to the SAME file plays "aid-admin".
    auto adminR = AuthDb::open(f.dbPath);
    ASSERT_TRUE(adminR.has_value()) << adminR.error().message;
    AuthDb adminDb = std::move(*adminR);
    FakeClock adminClock;
    adminClock.set(aid::Timestamp{std::chrono::seconds{1'700'000'000}});
    UserRepo admin{adminDb, adminClock};

    auto id = admin.create("alice", "old-hash");
    ASSERT_TRUE(id.has_value()) << id.error().message;

    // Reader observes the user. The extra lookupById exercises a second
    // cached statement — the multi-statement interleave that, before the
    // fix, kept at least one read transaction permanently open.
    auto first = reader.lookupByUsername("alice");
    ASSERT_TRUE(first.has_value()) << first.error().message;
    ASSERT_TRUE(first->has_value());
    EXPECT_EQ((*first)->passwordHash, "old-hash");
    ASSERT_TRUE(reader.lookupById(*id).has_value());

    // aid-admin resets the password on its own connection and commits.
    ASSERT_TRUE(admin.setPasswordHash(*id, "new-hash").has_value());

    // The daemon's long-lived connection must now see the new hash.
    auto second = reader.lookupByUsername("alice");
    ASSERT_TRUE(second.has_value()) << second.error().message;
    ASSERT_TRUE(second->has_value());
    EXPECT_EQ((*second)->passwordHash, "new-hash");
}
