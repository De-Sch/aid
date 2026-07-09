// aid-admin — CLI for managing the AID daemon's auth.db. Reuses
// aid_auth (AuthDb / UserRepo / SessionRepo / PasswordHasher) — does
// NOT depend on Drogon, does NOT speak HTTP. Operator shell access is
// the only path to user creation.
//
// Exit codes: 0 success, 2 usage error, 3 user not found,
// 4 user already exists, 5 SQLite/IO error.

#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/auth/AuthDb.h"
#include "aid/auth/PasswordHasher.h"
#include "aid/auth/SessionRepo.h"
#include "aid/auth/UserRepo.h"
#include "aid/crosscutting/Clock.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"
#include "aid/value-types/TimeFormat.h"

namespace {

constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitNotFound = 3;
constexpr int kExitConflict = 4;
constexpr int kExitIoError = 5;

constexpr std::size_t kMinPasswordLen = 8;

// Default DB path: matches the daemon's config default. Tests override
// with --db.
constexpr const char* kDefaultDbPath = "/var/lib/aid-daemon/auth.db";

struct Args {
    std::string dbPath{kDefaultDbPath};
    std::string command;
    std::optional<std::string> username;
    bool yes = false; // --yes, skip confirmation prompts
};

void usage() {
    std::fputs(
        R"(aid-admin — manage AID daemon users and sessions

Usage:
  aid-admin [--db <path>] add-user      --username <name>
  aid-admin [--db <path>] reset-password --username <name>
  aid-admin [--db <path>] delete-user   --username <name> [--yes]
  aid-admin [--db <path>] list-users
  aid-admin [--db <path>] list-sessions [--username <name>]
  aid-admin [--db <path>] revoke-all    --username <name> [--yes]
  aid-admin hash-recovery-key

`hash-recovery-key` prompts for a master/recovery key and prints its
Argon2id hash to stdout for pasting into config.json (Auth.recoveryKeyHash).
It touches no database, so it works on a fresh box before auth.db exists.

Default --db path: )",
        stderr);
    std::fputs(kDefaultDbPath, stderr);
    std::fputs("\n", stderr);
}

// Pull a string value from --flag <value> or --flag=value.
// Returns the consumed-argv-count (1 or 2) or 0 on miss.
[[nodiscard]] int tryReadFlag(int argc, char** argv, int idx, std::string_view flag,
                              std::string& out) {
    std::string_view arg{argv[idx]};
    if (arg == flag) {
        if (idx + 1 >= argc) {
            return -1;
        }
        out = argv[idx + 1];
        return 2;
    }
    const std::string eq = std::string{flag} + "=";
    if (arg.starts_with(eq)) {
        out = std::string{arg.substr(eq.size())};
        return 1;
    }
    return 0;
}

[[nodiscard]] std::optional<Args> parseArgs(int argc, char** argv) {
    Args a;
    int i = 1;
    while (i < argc) {
        std::string_view arg{argv[i]};
        std::string tmp;
        if (int n = tryReadFlag(argc, argv, i, "--db", tmp); n != 0) {
            if (n < 0) {
                return std::nullopt;
            }
            a.dbPath = tmp;
            i += n;
            continue;
        }
        if (int n = tryReadFlag(argc, argv, i, "--username", tmp); n != 0) {
            if (n < 0) {
                return std::nullopt;
            }
            a.username = tmp;
            i += n;
            continue;
        }
        if (arg == "--yes" || arg == "-y") {
            a.yes = true;
            ++i;
            continue;
        }
        if (arg.starts_with("--")) {
            return std::nullopt;
        }
        if (a.command.empty()) {
            a.command = std::string{arg};
            ++i;
            continue;
        }
        return std::nullopt; // unexpected positional
    }
    if (a.command.empty()) {
        return std::nullopt;
    }
    return a;
}

// SIGINT/SIGTERM-safe terminal state held in file-scope so the signal
// handler can restore ECHO if the user hits Ctrl-C mid-prompt: signal
// handlers must restore the terminal.
struct termios g_savedTermios {};
volatile sig_atomic_t g_termiosSaved = 0;

extern "C" void restoreTermiosOnSignal(int sig) {
    // Acquire-pair with the release fence in readPassword that publishes
    // the populated g_savedTermios before flipping g_termiosSaved.
    std::atomic_signal_fence(std::memory_order_acquire);
    if (g_termiosSaved != 0) {
        ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_savedTermios);
        g_termiosSaved = 0;
    }
    // Re-raise with the default disposition so the parent sees a
    // proper signal-exit (and ctest reports an interrupted run rather
    // than a hung one).
    ::signal(sig, SIG_DFL);
    ::raise(sig);
}

// Read a line from stdin, suppressing terminal echo if stdin is a TTY.
// On non-TTY (piped) input — used by the test harness — just reads
// the line normally so the password can be supplied without a fake PTY.
std::string readPassword(const char* prompt) {
    std::fputs(prompt, stdout);
    std::fflush(stdout);
    std::string line;
    if (::isatty(STDIN_FILENO) != 0) {
        struct termios newT {};
        if (::tcgetattr(STDIN_FILENO, &g_savedTermios) == 0) {
            // Publish the populated g_savedTermios before flipping the
            // visibility flag the signal handler reads.
            std::atomic_signal_fence(std::memory_order_release);
            g_termiosSaved = 1;
            const auto prevInt = ::signal(SIGINT, restoreTermiosOnSignal);
            const auto prevTerm = ::signal(SIGTERM, restoreTermiosOnSignal);
            newT = g_savedTermios;
            newT.c_lflag &= ~static_cast<tcflag_t>(ECHO);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &newT);
            std::getline(std::cin, line);
            ::tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_savedTermios);
            g_termiosSaved = 0;
            ::signal(SIGINT, prevInt);
            ::signal(SIGTERM, prevTerm);
            std::fputc('\n', stdout);
            return line;
        }
    }
    std::getline(std::cin, line);
    return line;
}

// minLen defaults to the user-password floor (8). hash-recovery-key
// passes 1 — the recovery key is operator-set for an internal tool and
// is allowed to be short (the operator's call), so only empty is rejected.
[[nodiscard]] std::string promptNewPassword(std::size_t minLen = kMinPasswordLen) {
    while (true) {
        const auto a = readPassword("Password: ");
        if (a.size() < minLen) {
            std::cerr << "Password must be at least " << minLen << " character(s).\n";
            continue;
        }
        const auto b = readPassword("Confirm:  ");
        if (a != b) {
            std::cerr << "Passwords do not match.\n";
            continue;
        }
        return a;
    }
}

// UTC ISO-8601 for the CLI's created/expires/last-seen columns — shares
// aid::formatIso8601Utc with the dashboard's updatedAt field.
[[nodiscard]] std::string formatTimestamp(aid::Timestamp t) {
    return aid::formatIso8601Utc(t);
}

[[nodiscard]] std::string formatOptionalTimestamp(const std::optional<aid::Timestamp>& t) {
    return t.has_value() ? formatTimestamp(*t) : std::string{"-"};
}

// Hash a recovery key for config.json. No DB involved — the only stdout
// line is the hash itself, so `aid-admin hash-recovery-key` is pasteable.
int cmdHashRecoveryKey() {
    aid::auth::PasswordHasher::initialize();
    const std::string key = promptNewPassword(1); // recovery key: any non-empty value
    auto hashRes = aid::auth::PasswordHasher::hash(key);
    if (!hashRes) {
        std::cerr << "hash failed: " << hashRes.error().message << "\n";
        return kExitIoError;
    }
    std::cout << *hashRes << "\n";
    return kExitOk;
}

int cmdAddUser(aid::auth::UserRepo& users, const Args& a) {
    if (!a.username) {
        usage();
        return kExitUsage;
    }
    const std::string pw = promptNewPassword();
    auto hashRes = aid::auth::PasswordHasher::hash(pw);
    if (!hashRes) {
        std::cerr << "hash failed: " << hashRes.error().message << "\n";
        return kExitIoError;
    }
    auto id = users.create(*a.username, *hashRes);
    if (!id) {
        if (id.error().code == aid::plumbing::ErrorCode::Conflict) {
            std::cerr << "user already exists: " << *a.username << "\n";
            return kExitConflict;
        }
        std::cerr << "create failed: " << id.error().message << "\n";
        return kExitIoError;
    }
    std::cout << "user " << *id << " created\n";
    return kExitOk;
}

int cmdResetPassword(aid::auth::UserRepo& users, aid::auth::SessionRepo& sessions, const Args& a) {
    if (!a.username) {
        usage();
        return kExitUsage;
    }
    auto by = users.lookupByUsername(*a.username);
    if (!by) {
        std::cerr << "lookup failed: " << by.error().message << "\n";
        return kExitIoError;
    }
    if (!by->has_value()) {
        std::cerr << "user not found: " << *a.username << "\n";
        return kExitNotFound;
    }
    const std::string pw = promptNewPassword();
    auto hashRes = aid::auth::PasswordHasher::hash(pw);
    if (!hashRes) {
        std::cerr << "hash failed: " << hashRes.error().message << "\n";
        return kExitIoError;
    }
    if (auto set = users.setPasswordHash((*by)->id, *hashRes); !set) {
        std::cerr << "setPasswordHash failed: " << set.error().message << "\n";
        return kExitIoError;
    }
    // Revoke every existing session so the old credential's cookie stops working.
    if (auto rv = sessions.revokeAllFor((*by)->id); !rv) {
        std::cerr << "revokeAllFor failed: " << rv.error().message << "\n";
        return kExitIoError;
    }
    std::cout << "password reset for " << *a.username << "\n";
    return kExitOk;
}

int cmdDeleteUser(aid::auth::UserRepo& users, const Args& a) {
    if (!a.username) {
        usage();
        return kExitUsage;
    }
    auto by = users.lookupByUsername(*a.username);
    if (!by) {
        std::cerr << "lookup failed: " << by.error().message << "\n";
        return kExitIoError;
    }
    if (!by->has_value()) {
        std::cerr << "user not found: " << *a.username << "\n";
        return kExitNotFound;
    }
    if (!a.yes) {
        std::cout << "Really delete '" << *a.username << "'? [y/N] ";
        std::cout.flush();
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "aborted\n";
            return kExitOk;
        }
    }
    if (auto del = users.deleteUser((*by)->id); !del) {
        std::cerr << "deleteUser failed: " << del.error().message << "\n";
        return kExitIoError;
    }
    std::cout << "deleted " << *a.username << "\n";
    return kExitOk;
}

int cmdListUsers(aid::auth::UserRepo& users) {
    auto all = users.listAll();
    if (!all) {
        std::cerr << "listAll failed: " << all.error().message << "\n";
        return kExitIoError;
    }
    std::cout << "id\tusername\tcreated_at\tlast_login_at\n";
    for (const auto& u : *all) {
        std::cout << u.id << "\t" << u.handle.v << "\t" << formatTimestamp(u.createdAt) << "\t"
                  << formatOptionalTimestamp(u.lastLoginAt) << "\n";
    }
    return kExitOk;
}

int cmdListSessions(aid::auth::UserRepo& users, aid::auth::SessionRepo& sessions, const Args& a) {
    using Sessions = std::vector<aid::auth::SessionRepo::Session>;
    aid::plumbing::Result<Sessions> rows{Sessions{}};
    if (a.username) {
        auto by = users.lookupByUsername(*a.username);
        if (!by) {
            std::cerr << "lookup failed: " << by.error().message << "\n";
            return kExitIoError;
        }
        if (!by->has_value()) {
            std::cerr << "user not found: " << *a.username << "\n";
            return kExitNotFound;
        }
        rows = sessions.listAllForUser((*by)->id);
    } else {
        rows = sessions.listAll();
    }
    if (!rows) {
        std::cerr << "list sessions failed: " << rows.error().message << "\n";
        return kExitIoError;
    }
    // Never print tokenHash.
    std::cout << "id\tuser_id\tprefix\tcreated_at\texpires_at\tlast_seen_at\tip\tua\n";
    for (const auto& s : *rows) {
        std::cout << s.id << "\t" << s.userId << "\t" << s.prefix << "\t"
                  << formatTimestamp(s.createdAt) << "\t" << formatTimestamp(s.expiresAt) << "\t"
                  << formatTimestamp(s.lastSeenAt) << "\t" << s.ipAtLogin.value_or("-") << "\t"
                  << s.userAgent.value_or("-") << "\n";
    }
    return kExitOk;
}

int cmdRevokeAll(aid::auth::UserRepo& users, aid::auth::SessionRepo& sessions, const Args& a) {
    if (!a.username) {
        usage();
        return kExitUsage;
    }
    auto by = users.lookupByUsername(*a.username);
    if (!by) {
        std::cerr << "lookup failed: " << by.error().message << "\n";
        return kExitIoError;
    }
    if (!by->has_value()) {
        std::cerr << "user not found: " << *a.username << "\n";
        return kExitNotFound;
    }
    if (!a.yes) {
        std::cout << "Revoke ALL sessions for '" << *a.username << "'? [y/N] ";
        std::cout.flush();
        std::string ans;
        std::getline(std::cin, ans);
        if (ans != "y" && ans != "Y") {
            std::cout << "aborted\n";
            return kExitOk;
        }
    }
    if (auto rv = sessions.revokeAllFor((*by)->id); !rv) {
        std::cerr << "revokeAllFor failed: " << rv.error().message << "\n";
        return kExitIoError;
    }
    std::cout << "all sessions revoked for " << *a.username << "\n";
    return kExitOk;
}

} // namespace

int main(int argc, char** argv) {
    // Restrict the file-creation mask BEFORE opening auth.db. SQLite
    // creates auth.db-wal and auth.db-shm lazily on the first write,
    // inheriting whatever umask the process has at that point — if it
    // were 022 (the typical login default) the WAL would be 0644, and
    // those files carry the same in-flight rows + password hashes that
    // we already refuse to expose via the 0600 check on auth.db itself.
    // 0077 → owner-only on every file this process creates. The
    // umask(2) return value is the previous mask; we discard it on
    // purpose — aid-admin owns its file-creation policy from here on.
    (void)::umask(0077);

    auto argsOpt = parseArgs(argc, argv);
    if (!argsOpt) {
        usage();
        return kExitUsage;
    }
    const Args& args = *argsOpt;

    // hash-recovery-key needs no database — dispatch it before AuthDb::open
    // so an operator can populate config.json on a fresh box before any
    // auth.db exists.
    if (args.command == "hash-recovery-key") {
        return cmdHashRecoveryKey();
    }

    auto dbRes = aid::auth::AuthDb::open(std::filesystem::path{args.dbPath});
    if (!dbRes) {
        std::cerr << "open auth.db failed: " << dbRes.error().message << "\n";
        return kExitIoError;
    }
    auto db = std::move(*dbRes);

    aid::crosscutting::RealClock clock;
    aid::crosscutting::AuthConfig cfg;
    aid::auth::UserRepo users{db, clock};
    aid::auth::SessionRepo sessions{db, clock, cfg};
    aid::auth::PasswordHasher::initialize();

    if (args.command == "add-user") {
        return cmdAddUser(users, args);
    }
    if (args.command == "reset-password") {
        return cmdResetPassword(users, sessions, args);
    }
    if (args.command == "delete-user") {
        return cmdDeleteUser(users, args);
    }
    if (args.command == "list-users") {
        return cmdListUsers(users);
    }
    if (args.command == "list-sessions") {
        return cmdListSessions(users, sessions, args);
    }
    if (args.command == "revoke-all") {
        return cmdRevokeAll(users, sessions, args);
    }
    usage();
    return kExitUsage;
}
