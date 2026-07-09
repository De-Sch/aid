#include <gtest/gtest.h>
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
#include "aid/auth/UserGate.h"
#include "aid/auth/UserRepo.h"
#include "aid/value-types/Ids.h"

namespace fs = std::filesystem;

using aid::UserHandle;
using aid::auth::AuthDb;
using aid::auth::userKnown;
using aid::auth::UserRepo;
using aid::fakes::FakeClock;

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
        name << "aid_usergate_" << pid << "_" << now << "_" << seq;
        dir = fs::temp_directory_path() / name.str();
        fs::create_directories(dir);
        dbPath = dir / "auth.db";

        auto r = AuthDb::open(dbPath);
        if (!r) {
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

TEST(UserGate, AbsentHandleIsAllowed) {
    Fixture f;
    auto repo = f.makeRepo();
    // An event that carries no user (std::nullopt) is not gated.
    EXPECT_TRUE(userKnown(repo, std::optional<UserHandle>{}));
}

TEST(UserGate, KnownUserIsAllowed) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create("alice", "$argon2id$dummyhash").has_value());

    EXPECT_TRUE(userKnown(repo, UserHandle{"alice"}));
    EXPECT_TRUE(userKnown(repo, std::optional<UserHandle>{UserHandle{"alice"}}));
}

TEST(UserGate, UnknownUserIsDropped) {
    Fixture f;
    auto repo = f.makeRepo();
    // No users created — any present handle is unknown.
    EXPECT_FALSE(userKnown(repo, UserHandle{"nobody"}));
    EXPECT_FALSE(userKnown(repo, std::optional<UserHandle>{UserHandle{"nobody"}}));
}

TEST(UserGate, DeletedUserBecomesUnknown) {
    Fixture f;
    auto repo = f.makeRepo();
    auto id = repo.create("bob", "$argon2id$dummyhash");
    ASSERT_TRUE(id.has_value()) << id.error().message;
    EXPECT_TRUE(userKnown(repo, UserHandle{"bob"}));

    ASSERT_TRUE(repo.deleteUser(*id).has_value());
    EXPECT_FALSE(userKnown(repo, UserHandle{"bob"}));
}

TEST(UserGate, MatchIsCaseSensitive) {
    Fixture f;
    auto repo = f.makeRepo();
    ASSERT_TRUE(repo.create("carol", "$argon2id$dummyhash").has_value());
    // lookupByUsername is an exact match; calls.py sends lowercase handles, so
    // a mixed-case account would not match (documented gotcha).
    EXPECT_TRUE(userKnown(repo, UserHandle{"carol"}));
    EXPECT_FALSE(userKnown(repo, UserHandle{"Carol"}));
}
