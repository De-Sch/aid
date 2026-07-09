// End-to-end test of the aid-admin CLI. Runs the actual binary as a
// subprocess against a temp DB, piping stdin for password prompts.
// AID_ADMIN_BINARY is wired in by the CMake target_compile_definitions.

#include <fcntl.h>
#include <gtest/gtest.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include "aid/auth/AuthDb.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"

namespace fs = std::filesystem;

using aid::auth::AuthDb;
using aid::auth::PasswordHasher;
using aid::auth::SessionRepo;
using aid::auth::UserRepo;
using aid::crosscutting::AuthConfig;
using aid::crosscutting::RealClock;

namespace {

struct ProcResult {
    int exitCode;
    std::string stdoutText;
};

// Runs the aid-admin binary with the given argv and stdin payload.
// Captures stdout. stderr is discarded (so test output stays clean
// when the CLI complains by design — e.g. user-not-found).
[[nodiscard]] ProcResult runAdmin(const std::vector<std::string>& argv, std::string_view stdinIn) {
    int outPipe[2]{-1, -1};
    int inPipe[2]{-1, -1};
    if (::pipe(outPipe) != 0 || ::pipe(inPipe) != 0) {
        ADD_FAILURE() << "pipe() failed";
        return {1, ""};
    }
    pid_t pid = ::fork();
    if (pid < 0) {
        ADD_FAILURE() << "fork() failed";
        return {1, ""};
    }
    if (pid == 0) {
        // Child.
        ::dup2(inPipe[0], STDIN_FILENO);
        ::dup2(outPipe[1], STDOUT_FILENO);
        // Redirect stderr to /dev/null. We don't assert on stderr.
        int devnull = ::open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            ::dup2(devnull, STDERR_FILENO);
            ::close(devnull);
        }
        ::close(inPipe[0]);
        ::close(inPipe[1]);
        ::close(outPipe[0]);
        ::close(outPipe[1]);

        std::vector<char*> args;
        args.reserve(argv.size() + 1);
        for (const auto& s : argv) {
            args.push_back(const_cast<char*>(s.c_str()));
        }
        args.push_back(nullptr);
        ::execv(AID_ADMIN_BINARY, args.data());
        std::_Exit(127);
    }
    // Parent.
    ::close(inPipe[0]);
    ::close(outPipe[1]);
    if (!stdinIn.empty()) {
        const ssize_t w = ::write(inPipe[1], stdinIn.data(), stdinIn.size());
        (void)w;
    }
    ::close(inPipe[1]);

    std::string out;
    char buf[4096];
    while (true) {
        const ssize_t n = ::read(outPipe[0], buf, sizeof(buf));
        if (n <= 0) {
            break;
        }
        out.append(buf, static_cast<std::size_t>(n));
    }
    ::close(outPipe[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    const int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return {code, std::move(out)};
}

struct Fixture {
    fs::path dir;
    fs::path dbPath;

    Fixture() {
        static std::atomic<std::uint64_t> counter{0};
        const auto pid = static_cast<std::uint64_t>(::getpid());
        const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
        const auto now =
            static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        std::ostringstream name;
        name << "aid_admin_cli_" << pid << "_" << now << "_" << seq;
        dir = fs::temp_directory_path() / name.str();
        fs::create_directories(dir);
        dbPath = dir / "auth.db";
        // Seed an empty DB so each test starts from a known migration state.
        {
            auto r = AuthDb::open(dbPath);
            EXPECT_TRUE(r.has_value()) << (r.has_value() ? "" : r.error().message);
        }
    }
    ~Fixture() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }

    [[nodiscard]] bool userExists(std::string_view name) {
        RealClock clock;
        auto r = AuthDb::open(dbPath);
        EXPECT_TRUE(r.has_value());
        UserRepo users{*r, clock};
        auto by = users.lookupByUsername(name);
        EXPECT_TRUE(by.has_value());
        return by->has_value();
    }
};

} // namespace

TEST(AdminCli, NoArgsExitsUsage) {
    auto res = runAdmin({"aid-admin"}, "");
    EXPECT_EQ(res.exitCode, 2);
}

TEST(AdminCli, UnknownCommandExitsUsage) {
    Fixture f;
    auto res = runAdmin({"aid-admin", "--db", f.dbPath.string(), "frobnicate"}, "");
    EXPECT_EQ(res.exitCode, 2);
}

TEST(AdminCli, AddUserCreatesUser) {
    Fixture f;
    auto res = runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                        "password123\npassword123\n");
    EXPECT_EQ(res.exitCode, 0) << res.stdoutText;
    EXPECT_NE(res.stdoutText.find("user "), std::string::npos);
    EXPECT_TRUE(f.userExists("alice"));
}

TEST(AdminCli, AddUserRejectsTooShortPassword) {
    Fixture f;
    // 7 chars first, then a valid 8-char retry.
    auto res = runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                        "short\nshort\nlonger12\nlonger12\n");
    EXPECT_EQ(res.exitCode, 0);
    EXPECT_TRUE(f.userExists("alice"));
}

TEST(AdminCli, AddUserDuplicateExitsConflict) {
    Fixture f;
    {
        auto first =
            runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                     "password123\npassword123\n");
        ASSERT_EQ(first.exitCode, 0);
    }
    auto dup = runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                        "password123\npassword123\n");
    EXPECT_EQ(dup.exitCode, 4);
}

TEST(AdminCli, ResetPasswordSucceedsAndRevokesSessions) {
    Fixture f;
    // Seed user + session via the libs directly so we can verify the
    // CLI deleted the session row.
    AuthConfig cfg;
    RealClock clock;
    PasswordHasher::initialize();
    {
        auto r = AuthDb::open(f.dbPath);
        ASSERT_TRUE(r.has_value());
        UserRepo users{*r, clock};
        SessionRepo sessions{*r, clock, cfg};
        auto h = PasswordHasher::hash("oldpassword");
        ASSERT_TRUE(h.has_value());
        auto id = users.create("alice", *h);
        ASSERT_TRUE(id.has_value());
        auto sess = sessions.create(*id, "deadbeef" + std::string(56, 'a'), "deadbeef", "ip", "ua");
        ASSERT_TRUE(sess.has_value());
    }

    auto res =
        runAdmin({"aid-admin", "--db", f.dbPath.string(), "reset-password", "--username", "alice"},
                 "newpassword\nnewpassword\n");
    EXPECT_EQ(res.exitCode, 0) << res.stdoutText;

    auto r = AuthDb::open(f.dbPath);
    ASSERT_TRUE(r.has_value());
    SessionRepo sessions{*r, clock, cfg};
    auto sess = sessions.listAll();
    ASSERT_TRUE(sess.has_value());
    EXPECT_TRUE(sess->empty());
}

TEST(AdminCli, ResetPasswordOnMissingUserExitsNotFound) {
    Fixture f;
    auto res = runAdmin(
        {"aid-admin", "--db", f.dbPath.string(), "reset-password", "--username", "ghost"}, "");
    EXPECT_EQ(res.exitCode, 3);
}

TEST(AdminCli, ListUsersReturnsCreated) {
    Fixture f;
    {
        auto add =
            runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                     "password123\npassword123\n");
        ASSERT_EQ(add.exitCode, 0);
    }
    auto res = runAdmin({"aid-admin", "--db", f.dbPath.string(), "list-users"}, "");
    EXPECT_EQ(res.exitCode, 0);
    EXPECT_NE(res.stdoutText.find("alice"), std::string::npos);
}

TEST(AdminCli, DeleteUserWithYesFlagRemovesRow) {
    Fixture f;
    {
        auto add =
            runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                     "password123\npassword123\n");
        ASSERT_EQ(add.exitCode, 0);
    }
    auto res = runAdmin(
        {"aid-admin", "--db", f.dbPath.string(), "delete-user", "--username", "alice", "--yes"},
        "");
    EXPECT_EQ(res.exitCode, 0);
    EXPECT_FALSE(f.userExists("alice"));
}

TEST(AdminCli, DeleteUserMissingExitsNotFound) {
    Fixture f;
    auto res = runAdmin(
        {"aid-admin", "--db", f.dbPath.string(), "delete-user", "--username", "ghost", "--yes"},
        "");
    EXPECT_EQ(res.exitCode, 3);
}

TEST(AdminCli, RevokeAllRemovesUserSessions) {
    Fixture f;
    AuthConfig cfg;
    RealClock clock;
    PasswordHasher::initialize();
    {
        auto r = AuthDb::open(f.dbPath);
        ASSERT_TRUE(r.has_value());
        UserRepo users{*r, clock};
        SessionRepo sessions{*r, clock, cfg};
        auto h = PasswordHasher::hash("p");
        auto id = users.create("alice", *h);
        ASSERT_TRUE(id.has_value());
        auto s1 = sessions.create(*id, "h" + std::string(63, '1'), "p1111111", "", "");
        auto s2 = sessions.create(*id, "h" + std::string(63, '2'), "p2222222", "", "");
        ASSERT_TRUE(s1.has_value());
        ASSERT_TRUE(s2.has_value());
    }
    auto res = runAdmin(
        {"aid-admin", "--db", f.dbPath.string(), "revoke-all", "--username", "alice", "--yes"}, "");
    EXPECT_EQ(res.exitCode, 0);

    auto r = AuthDb::open(f.dbPath);
    ASSERT_TRUE(r.has_value());
    SessionRepo sessions{*r, clock, cfg};
    auto sess = sessions.listAll();
    ASSERT_TRUE(sess.has_value());
    EXPECT_TRUE(sess->empty());
}

TEST(AdminCli, RevokeAllWithoutYesAndAnswerNoAborts) {
    Fixture f;
    AuthConfig cfg;
    RealClock clock;
    PasswordHasher::initialize();
    {
        auto r = AuthDb::open(f.dbPath);
        ASSERT_TRUE(r.has_value());
        UserRepo users{*r, clock};
        SessionRepo sessions{*r, clock, cfg};
        auto h = PasswordHasher::hash("p");
        auto id = users.create("alice", *h);
        ASSERT_TRUE(id.has_value());
        ASSERT_TRUE(
            sessions.create(*id, "h" + std::string(63, '1'), "p1111111", "", "").has_value());
    }
    // Pipe "n\n" — should abort and exit 0 ("aborted"), session left intact.
    auto res = runAdmin(
        {"aid-admin", "--db", f.dbPath.string(), "revoke-all", "--username", "alice"}, "n\n");
    EXPECT_EQ(res.exitCode, 0);
    EXPECT_NE(res.stdoutText.find("aborted"), std::string::npos);

    auto r = AuthDb::open(f.dbPath);
    ASSERT_TRUE(r.has_value());
    SessionRepo sessions{*r, clock, cfg};
    auto sess = sessions.listAll();
    ASSERT_TRUE(sess.has_value());
    EXPECT_EQ(sess->size(), 1U) << "session must survive the aborted revoke-all";
}

// Extract the Argon2id hash line from stdout. The prompts ("Password: ",
// "Confirm:  ") also land on stdout, so the hash is the "$argon2id$…"
// token; return it without trailing whitespace.
[[nodiscard]] std::string extractArgonHash(const std::string& out) {
    const auto pos = out.find("$argon2");
    if (pos == std::string::npos) {
        return {};
    }
    auto end = out.find('\n', pos);
    if (end == std::string::npos) {
        end = out.size();
    }
    return out.substr(pos, end - pos);
}

TEST(AdminCli, HashRecoveryKeyPrintsVerifiableArgon2Hash) {
    auto res = runAdmin({"aid-admin", "hash-recovery-key"}, "master-key-12\nmaster-key-12\n");
    EXPECT_EQ(res.exitCode, 0) << res.stdoutText;

    const std::string hash = extractArgonHash(res.stdoutText);
    ASSERT_FALSE(hash.empty()) << "no $argon2 hash on stdout: " << res.stdoutText;

    PasswordHasher::initialize();
    EXPECT_TRUE(PasswordHasher::verify("master-key-12", hash));
    EXPECT_FALSE(PasswordHasher::verify("wrong-key-xyz", hash));
}

// hash-recovery-key must work with no auth.db present (no --db flag, and
// the default path is never opened — the command is dispatched before
// AuthDb::open). Proves an operator can populate config.json on a fresh
// box before the database exists.
TEST(AdminCli, HashRecoveryKeyNeedsNoDatabase) {
    auto res = runAdmin({"aid-admin", "hash-recovery-key"}, "another-key-9\nanother-key-9\n");
    EXPECT_EQ(res.exitCode, 0) << res.stdoutText;
    EXPECT_NE(res.stdoutText.find("$argon2"), std::string::npos);
}

// L2 — after aid-admin opens + writes the auth.db, the SQLite
// companion files (-wal, -shm) must inherit the restricted umask the
// binary sets at the top of main(). Without the umask change the WAL
// would land at 0644 on a typical login default and carry in-flight
// password hashes alongside auth.db itself.
TEST(AdminCli, AuthDbAndCompanionFilesAre0600AfterWrite) {
    Fixture f;
    // The fixture pre-opens auth.db once; clean up the companion
    // files so the aid-admin subprocess is the sole creator.
    std::error_code ec;
    std::filesystem::remove(f.dbPath.string() + "-wal", ec);
    std::filesystem::remove(f.dbPath.string() + "-shm", ec);

    auto res = runAdmin({"aid-admin", "--db", f.dbPath.string(), "add-user", "--username", "alice"},
                        "password123\npassword123\n");
    ASSERT_EQ(res.exitCode, 0) << res.stdoutText;

    bool sawAny = false;
    for (const auto& entry : std::filesystem::directory_iterator(f.dir)) {
        const std::string name = entry.path().filename().string();
        if (name.rfind("auth.db", 0) != 0) {
            continue;
        }
        struct ::stat st {};
        ASSERT_EQ(::stat(entry.path().c_str(), &st), 0) << name;
        EXPECT_EQ(st.st_mode & 0777U, 0600U) << "wide permissions on " << name << " (got 0"
                                             << std::oct << (st.st_mode & 0777U) << ")";
        sawAny = true;
    }
    EXPECT_TRUE(sawAny) << "no auth.db* files found in temp dir";
}
