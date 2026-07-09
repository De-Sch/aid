#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::crosscutting::AuthConfig;
using aid::crosscutting::Config;
using aid::crosscutting::expandConfigPath;
using aid::crosscutting::LoggerConfig;
using aid::crosscutting::TicketSystemConfig;
using aid::crosscutting::UiConfig;
using aid::plumbing::ErrorCode;

namespace {

// Owner-mismatch (st.st_uid != euid && st.st_uid != 0) is not
// exercisable from a non-root unit test runner because we can't chown
// to another uid; the code path is reachable via root-only chown and is
// covered by integration tests when those land.

struct ConfigFile {
    fs::path dir;
    fs::path path;

    ~ConfigFile() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

ConfigFile makeConfigFile(std::string_view body, mode_t mode) {
    static std::atomic<std::uint64_t> counter{0};
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    std::ostringstream name;
    name << "aid_cfg_" << pid << "_" << now << "_" << seq;

    ConfigFile cf;
    cf.dir = fs::temp_directory_path() / name.str();
    fs::create_directories(cf.dir);
    cf.path = cf.dir / "config.json";

    {
        std::ofstream out(cf.path);
        out << body;
    }
    // chmod after write — umask on the open() above may set bits.
    EXPECT_EQ(::chmod(cf.path.c_str(), mode), 0);
    return cf;
}

constexpr std::string_view kFullValidBody = R"({
    "Logger": {
        "level": "INFO",
        "backendLogPath": "/tmp/backend.log",
        "frontendLogPath": "/tmp/frontend.log"
    },
    "Auth": {
        "dbPath": "/tmp/auth.db",
        "sessionLifetimeSeconds": 60,
        "cookieName": "test_cookie",
        "cookieSecure": false,
        "maxConcurrentLogins": 2,
        "trustForwardedFor": true,
        "trustedProxyAddresses": ["10.0.0.1", "127.0.0.1"]
    }
})";

} // namespace

TEST(Config, LoadAcceptsModeZeroSixFourZeroFileOwnedByCaller) {
    auto cf = makeConfigFile(kFullValidBody, 0640);
    auto r = Config::load(cf.path.string());
    ASSERT_TRUE(r.has_value()) << r.error().message;
}

TEST(Config, LoadAcceptsModeZeroSixZeroZero) {
    auto cf = makeConfigFile(kFullValidBody, 0600);
    auto r = Config::load(cf.path.string());
    EXPECT_TRUE(r.has_value()) << r.error().message;
}

TEST(Config, LoadRejectsWorldReadableConfig) {
    auto cf = makeConfigFile(kFullValidBody, 0644);
    auto r = Config::load(cf.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("wider than 0640"), std::string::npos);
}

TEST(Config, LoadRejectsGroupWritableConfig) {
    auto cf = makeConfigFile(kFullValidBody, 0660);
    auto r = Config::load(cf.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("wider than 0640"), std::string::npos);
}

TEST(Config, LoadRejectsMissingFile) {
    const fs::path nonexistent = fs::temp_directory_path() / "aid_cfg_does_not_exist_xyz";
    std::error_code ec;
    fs::remove(nonexistent, ec);
    auto r = Config::load(nonexistent.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("cannot open"), std::string::npos);
}

TEST(Config, LoadRejectsSymlinkTarget) {
    // Build a valid 0640 target, then point a symlink at it. The
    // O_NOFOLLOW open should refuse the symlink before we ever stat
    // the target. Closes the read-through-symlink hole on the
    // secrets-at-rest config file.
    auto target = makeConfigFile(kFullValidBody, 0640);
    const fs::path link = target.dir / "link.json";
    ASSERT_EQ(::symlink(target.path.c_str(), link.c_str()), 0);

    auto r = Config::load(link.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("symlink"), std::string::npos);
}

TEST(Config, LoadRejectsMalformedJson) {
    auto cf = makeConfigFile("{ not valid json", 0640);
    auto r = Config::load(cf.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("parse error"), std::string::npos);
}

TEST(Config, LoadRejectsNonObjectTopLevel) {
    auto cf = makeConfigFile("[1, 2, 3]", 0640);
    auto r = Config::load(cf.path.string());
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("must be an object"), std::string::npos);
}

TEST(Config, LoggerReturnsParsedSliceForAllRequiredKeys) {
    auto cf = makeConfigFile(kFullValidBody, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto lg = cfg->logger();
    ASSERT_TRUE(lg.has_value()) << lg.error().message;
    EXPECT_EQ(lg->level, "INFO");
    EXPECT_EQ(lg->backendLogPath, "/tmp/backend.log");
    EXPECT_EQ(lg->frontendLogPath, "/tmp/frontend.log");
}

TEST(Config, LoggerReportsMissingRequiredKey) {
    constexpr std::string_view body = R"({
        "Logger": {
            "backendLogPath": "/tmp/backend.log",
            "frontendLogPath": "/tmp/frontend.log"
        }
    })";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto lg = cfg->logger();
    ASSERT_FALSE(lg.has_value());
    EXPECT_EQ(lg.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(lg.error().message.find("Logger.level"), std::string::npos);
}

TEST(Config, LoggerReportsMissingSectionEntirely) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto lg = cfg->logger();
    ASSERT_FALSE(lg.has_value());
    EXPECT_NE(lg.error().message.find("Logger section"), std::string::npos);
}

TEST(Config, AuthAppliesDefaultsForOmittedKeys) {
    auto cf = makeConfigFile(R"({"Auth": {}})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_TRUE(a.has_value()) << a.error().message;
    EXPECT_EQ(a->dbPath, std::filesystem::path{"/var/lib/aid-daemon/auth.db"});
    EXPECT_EQ(a->sessionLifetimeSeconds, 2592000);
    EXPECT_EQ(a->cookieName, "aid_session");
    EXPECT_TRUE(a->cookieSecure);
}

TEST(Config, AuthAppliesDefaultsWhenSectionAbsent) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(a->sessionLifetimeSeconds, 2592000);
    EXPECT_TRUE(a->cookieSecure);
    EXPECT_EQ(a->maxConcurrentLogins, 4);
    EXPECT_FALSE(a->trustForwardedFor);
    EXPECT_TRUE(a->trustedProxyAddresses.empty());
}

TEST(Config, AuthOverridesAreApplied) {
    auto cf = makeConfigFile(kFullValidBody, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_TRUE(a.has_value()) << a.error().message;
    EXPECT_EQ(a->dbPath, std::filesystem::path{"/tmp/auth.db"});
    EXPECT_EQ(a->sessionLifetimeSeconds, 60);
    EXPECT_EQ(a->cookieName, "test_cookie");
    EXPECT_FALSE(a->cookieSecure);
    EXPECT_EQ(a->maxConcurrentLogins, 2);
    EXPECT_TRUE(a->trustForwardedFor);
    ASSERT_EQ(a->trustedProxyAddresses.size(), 2U);
    EXPECT_EQ(a->trustedProxyAddresses[0], "10.0.0.1");
    EXPECT_EQ(a->trustedProxyAddresses[1], "127.0.0.1");
}

TEST(Config, AuthRejectsTrustedProxyAddressesNotArray) {
    constexpr std::string_view body = R"({"Auth": {"trustedProxyAddresses": "not-an-array"}})";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(a.error().message.find("trustedProxyAddresses"), std::string::npos);
}

TEST(Config, AuthRejectsTrustForwardedForOnWithEmptyAllowlist) {
    constexpr std::string_view body =
        R"({"Auth": {"trustForwardedFor": true, "trustedProxyAddresses": []}})";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(a.error().message.find("trustForwardedFor"), std::string::npos);
}

TEST(Config, AuthRejectsTrustedProxyAddressesNonStringEntry) {
    constexpr std::string_view body = R"({"Auth": {"trustedProxyAddresses": ["10.0.0.1", 42]}})";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(a.error().message.find("trustedProxyAddresses"), std::string::npos);
}

TEST(Config, AuthRejectsIntegerOverflow) {
    // Larger than INT_MAX (2^31 - 1) on every platform we ship on.
    constexpr std::string_view body = R"({"Auth": {"sessionLifetimeSeconds": 9999999999}})";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(a.error().message.find("out of int range"), std::string::npos);
}

TEST(Config, AuthRejectsWrongTypedField) {
    constexpr std::string_view body = R"({"Auth": {"sessionLifetimeSeconds": "not-an-int"}})";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto a = cfg->auth();
    ASSERT_FALSE(a.has_value());
    EXPECT_EQ(a.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(a.error().message.find("sessionLifetimeSeconds"), std::string::npos);
}

namespace {

constexpr std::string_view kTicketSystemValidBody = R"({
    "TicketSystem": {
        "baseUrl": "http://op.example.com",
        "apiToken": "secret-token",
        "statusNew":        "1",
        "statusInProgress": "2",
        "statusClosed":     "3",
        "typeCall":            "Call",
        "projectNames": {
            "11": "Acme",
            "12": "Support"
        }
    },
    "Ui": {}
})";

} // namespace

TEST(Config, TicketSystemParsesAllRequiredFields) {
    auto cf = makeConfigFile(kTicketSystemValidBody, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    auto op = cfg->ticketSystem();
    ASSERT_TRUE(op.has_value()) << op.error().message;
    EXPECT_EQ(op->baseUrl, "http://op.example.com");
    EXPECT_EQ(op->apiToken, "secret-token");
    EXPECT_EQ(op->statusNew.v, "1");
    EXPECT_EQ(op->statusInProgress.v, "2");
    EXPECT_EQ(op->statusClosed.v, "3");
    EXPECT_EQ(op->typeCall, "Call");
    EXPECT_EQ(op->projectNames.size(), 2U);
    EXPECT_EQ(op->projectNames.at(aid::ProjectId{"11"}), "Acme");
    EXPECT_EQ(op->projectNames.at(aid::ProjectId{"12"}), "Support");
}

TEST(Config, TicketSystemMissingSectionFails) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto op = cfg->ticketSystem();
    ASSERT_FALSE(op.has_value());
    EXPECT_NE(op.error().message.find("TicketSystem section"), std::string::npos);
}

TEST(Config, TicketSystemMissingApiTokenFails) {
    constexpr std::string_view body = R"({
        "TicketSystem": {
            "baseUrl": "http://op.example.com",
            "statusNew": "1",
            "statusInProgress": "2",
            "statusClosed": "3",
            "typeCall": "Call"
        }
    })";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto op = cfg->ticketSystem();
    ASSERT_FALSE(op.has_value());
    EXPECT_NE(op.error().message.find("TicketSystem.apiToken"), std::string::npos);
}

TEST(Config, TicketSystemAcceptsEmptyProjectNames) {
    constexpr std::string_view body = R"({
        "TicketSystem": {
            "baseUrl": "http://op.example.com",
            "apiToken": "t",
            "statusNew": "1", "statusInProgress": "2", "statusClosed": "3",
            "typeCall": "Call"
        }
    })";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto op = cfg->ticketSystem();
    ASSERT_TRUE(op.has_value()) << op.error().message;
    EXPECT_TRUE(op->projectNames.empty());
}

TEST(Config, TicketSystemRejectsNonObjectProjectNames) {
    constexpr std::string_view body = R"({
        "TicketSystem": {
            "baseUrl": "x", "apiToken": "t",
            "statusNew": "1", "statusInProgress": "2", "statusClosed": "3",
            "typeCall": "Call",
            "projectNames": [1, 2, 3]
        }
    })";
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto op = cfg->ticketSystem();
    ASSERT_FALSE(op.has_value());
    EXPECT_NE(op.error().message.find("projectNames must be an object"), std::string::npos);
}

TEST(Config, UiMissingSectionFails) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto ui = cfg->ui();
    ASSERT_FALSE(ui.has_value());
    EXPECT_NE(ui.error().message.find("Ui section"), std::string::npos);
}

TEST(Config, UiDocumentRootAbsentIsNullopt) {
    // documentRoot is optional: the existing valid body omits it, so the
    // parsed slice must carry no path (API-only / Vite-proxy dev mode).
    auto cf = makeConfigFile(kTicketSystemValidBody, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto ui = cfg->ui();
    ASSERT_TRUE(ui.has_value()) << ui.error().message;
    EXPECT_FALSE(ui->documentRoot.has_value());
}

TEST(Config, UiParsesDocumentRootWhenPresent) {
    auto cf = makeConfigFile(R"({
        "Ui": {
            "documentRoot": "/srv/aid/ui/build"
        }
    })",
                             0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto ui = cfg->ui();
    ASSERT_TRUE(ui.has_value()) << ui.error().message;
    ASSERT_TRUE(ui->documentRoot.has_value());
    EXPECT_EQ(*ui->documentRoot, std::filesystem::path{"/srv/aid/ui/build"});
}

TEST(Config, UiRejectsNonStringDocumentRoot) {
    auto cf = makeConfigFile(R"({
        "Ui": {
            "documentRoot": 42
        }
    })",
                             0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto ui = cfg->ui();
    ASSERT_FALSE(ui.has_value());
    EXPECT_NE(ui.error().message.find("Ui.documentRoot must be a string"), std::string::npos);
}

TEST(Config, SectionJsonReturnsRawSliceForExistingSection) {
    auto cf = makeConfigFile(kTicketSystemValidBody, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto slice = cfg->sectionJson("TicketSystem");
    ASSERT_TRUE(slice.has_value());
    ASSERT_TRUE(slice->has_value());
    // The dump is compact JSON; assert on a substring known to be present.
    EXPECT_NE((**slice).find("\"apiToken\":\"secret-token\""), std::string::npos);
    EXPECT_NE((**slice).find("\"projectNames\""), std::string::npos);
}

TEST(Config, SectionJsonReturnsEmptyOptionalForMissingSection) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto slice = cfg->sectionJson("TicketSystem");
    ASSERT_TRUE(slice.has_value());
    EXPECT_FALSE(slice->has_value());
}

TEST(Config, ListenPortDefaultsToEightyWhenAbsent) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto port = cfg->listenPort();
    ASSERT_TRUE(port.has_value()) << port.error().message;
    EXPECT_EQ(*port, 80);
}

TEST(Config, ListenPortParsesExplicitValue) {
    auto cf = makeConfigFile(R"({"listenPort": 8088})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto port = cfg->listenPort();
    ASSERT_TRUE(port.has_value()) << port.error().message;
    EXPECT_EQ(*port, 8088);
}

TEST(Config, ListenPortRejectsNonInteger) {
    auto cf = makeConfigFile(R"({"listenPort": "8088"})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto port = cfg->listenPort();
    ASSERT_FALSE(port.has_value());
    EXPECT_EQ(port.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(port.error().message.find("listenPort"), std::string::npos);
}

TEST(Config, ListenPortRejectsOutOfRange) {
    auto cf = makeConfigFile(R"({"listenPort": 70000})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto port = cfg->listenPort();
    ASSERT_FALSE(port.has_value());
    EXPECT_EQ(port.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(port.error().message.find("[1, 65535]"), std::string::npos);
}

TEST(Config, ListenPortRejectsZero) {
    auto cf = makeConfigFile(R"({"listenPort": 0})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto port = cfg->listenPort();
    ASSERT_FALSE(port.has_value());
    EXPECT_EQ(port.error().code, ErrorCode::InvalidInput);
}

TEST(Config, WalPathDefaultsToProdInboxWhenAbsent) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto wp = cfg->walPath();
    ASSERT_TRUE(wp.has_value()) << wp.error().message;
    EXPECT_EQ(wp->string(), "/var/lib/aid-daemon/inbox.log");
}

TEST(Config, WalPathParsesExplicitAbsoluteValue) {
    auto cf = makeConfigFile(R"({"walPath": "/srv/aid/inbox.log"})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto wp = cfg->walPath();
    ASSERT_TRUE(wp.has_value()) << wp.error().message;
    EXPECT_EQ(wp->string(), "/srv/aid/inbox.log");
}

TEST(Config, WalPathExpandsEnvVar) {
    ::setenv("AID_TEST_WAL_ROOT", "/tmp/aidwal", 1);
    auto cf = makeConfigFile(R"({"walPath": "${AID_TEST_WAL_ROOT}/var/lib/inbox.log"})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto wp = cfg->walPath();
    ASSERT_TRUE(wp.has_value()) << wp.error().message;
    EXPECT_EQ(wp->string(), "/tmp/aidwal/var/lib/inbox.log");
    ::unsetenv("AID_TEST_WAL_ROOT");
}

TEST(Config, WalPathRejectsNonString) {
    auto cf = makeConfigFile(R"({"walPath": 123})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto wp = cfg->walPath();
    ASSERT_FALSE(wp.has_value());
    EXPECT_EQ(wp.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(wp.error().message.find("walPath"), std::string::npos);
}

TEST(Config, MembershipPollIntervalDefaultsToThirtyWhenAbsent) {
    auto cf = makeConfigFile("{}", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_TRUE(secs.has_value()) << secs.error().message;
    EXPECT_EQ(*secs, 30);
}

TEST(Config, MembershipPollIntervalParsesExplicitValue) {
    auto cf = makeConfigFile(R"({"membershipPollIntervalSec": 60})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_TRUE(secs.has_value()) << secs.error().message;
    EXPECT_EQ(*secs, 60);
}

TEST(Config, MembershipPollIntervalZeroDisablesUnchanged) {
    auto cf = makeConfigFile(R"({"membershipPollIntervalSec": 0})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_TRUE(secs.has_value()) << secs.error().message;
    EXPECT_EQ(*secs, 0); // explicit "disabled" sentinel, passed through.
}

TEST(Config, MembershipPollIntervalClampsBelowFloorUp) {
    auto cf = makeConfigFile(R"({"membershipPollIntervalSec": 1})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_TRUE(secs.has_value()) << secs.error().message;
    EXPECT_EQ(*secs, 5); // a positive value below the 5 s floor is clamped up.
}

TEST(Config, MembershipPollIntervalRejectsNegative) {
    auto cf = makeConfigFile(R"({"membershipPollIntervalSec": -1})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_FALSE(secs.has_value());
    EXPECT_EQ(secs.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(secs.error().message.find("membershipPollIntervalSec"), std::string::npos);
}

TEST(Config, MembershipPollIntervalRejectsNonInteger) {
    auto cf = makeConfigFile(R"({"membershipPollIntervalSec": "30"})", 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto secs = cfg->membershipPollIntervalSec();
    ASSERT_FALSE(secs.has_value());
    EXPECT_EQ(secs.error().code, ErrorCode::InvalidInput);
}

// --- cookieSecure-vs-listener cross-check -------------------------

using aid::crosscutting::isLoopbackInterface;
using aid::crosscutting::sessionCookieExposedOnLan;

TEST(LoopbackInterface, TreatsIpv4LoopbackBlockAsLoopback) {
    EXPECT_TRUE(isLoopbackInterface("127.0.0.1"));
    EXPECT_TRUE(isLoopbackInterface("127.0.0.0"));  // network address of the /8
    EXPECT_TRUE(isLoopbackInterface("127.0.0.53")); // anywhere in 127.0.0.0/8
    EXPECT_TRUE(isLoopbackInterface("127.255.255.255"));
}

TEST(LoopbackInterface, TreatsIpv6LoopbackAndLocalhostAsLoopback) {
    EXPECT_TRUE(isLoopbackInterface("::1"));
    EXPECT_TRUE(isLoopbackInterface("0:0:0:0:0:0:0:1")); // expanded ::1
    EXPECT_TRUE(isLoopbackInterface("localhost"));
}

TEST(LoopbackInterface, TreatsRoutableAndWildcardBindsAsNonLoopback) {
    EXPECT_FALSE(isLoopbackInterface("0.0.0.0")); // binds everywhere incl. LAN
    EXPECT_FALSE(isLoopbackInterface("::"));
    EXPECT_FALSE(isLoopbackInterface("192.168.1.10"));
    EXPECT_FALSE(isLoopbackInterface("10.0.0.5"));
    EXPECT_FALSE(isLoopbackInterface("")); // unparseable → conservative
    EXPECT_FALSE(isLoopbackInterface("not-an-ip"));
    // IPv4-mapped IPv6 loopback: documented conservative false-negative — it
    // isn't byte-equal to ::1, so it's classified non-loopback (over-warns
    // rather than silently suppressing). Drogon never binds this literal.
    EXPECT_FALSE(isLoopbackInterface("::ffff:127.0.0.1"));
}

// Acceptance case 1: cookieSecure=false on a non-loopback bind must trigger.
TEST(SessionCookieExposure, WarnsOnInsecureCookieOverNonLoopbackBind) {
    EXPECT_TRUE(sessionCookieExposedOnLan(/*cookieSecure=*/false, "192.168.1.10"));
    EXPECT_TRUE(sessionCookieExposedOnLan(/*cookieSecure=*/false, "0.0.0.0"));
}

// Acceptance case 2: loopback-only insecure dev must start clean.
TEST(SessionCookieExposure, AllowsInsecureCookieOnLoopbackBind) {
    EXPECT_FALSE(sessionCookieExposedOnLan(/*cookieSecure=*/false, "127.0.0.1"));
    EXPECT_FALSE(sessionCookieExposedOnLan(/*cookieSecure=*/false, "::1"));
}

// A secure cookie is never flagged regardless of the bind interface.
TEST(SessionCookieExposure, NeverWarnsWhenCookieSecure) {
    EXPECT_FALSE(sessionCookieExposedOnLan(/*cookieSecure=*/true, "192.168.1.10"));
    EXPECT_FALSE(sessionCookieExposedOnLan(/*cookieSecure=*/true, "127.0.0.1"));
}

// --- expandConfigPath (~ and ${VAR} expansion in config paths) ---------------

TEST(ExpandConfigPath, LeavesPlainAbsolutePathUnchanged) {
    auto r = expandConfigPath("/var/lib/aid-daemon/auth.db");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "/var/lib/aid-daemon/auth.db");
}

TEST(ExpandConfigPath, EmptyStringStaysEmpty) {
    auto r = expandConfigPath("");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "");
}

TEST(ExpandConfigPath, ExpandsLeadingTildeToHome) {
    ASSERT_EQ(::setenv("HOME", "/home/tester", 1), 0);
    auto slash = expandConfigPath("~/aid-dev/var/log/backend.log");
    ASSERT_TRUE(slash.has_value()) << slash.error().message;
    EXPECT_EQ(*slash, "/home/tester/aid-dev/var/log/backend.log");

    auto bare = expandConfigPath("~");
    ASSERT_TRUE(bare.has_value());
    EXPECT_EQ(*bare, "/home/tester");
}

TEST(ExpandConfigPath, TildeIsNotSpecialMidPath) {
    // Only a LEADING ~ (or ~/) expands; a ~ elsewhere is a literal char.
    auto r = expandConfigPath("/opt/~backup/x");
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(*r, "/opt/~backup/x");
}

TEST(ExpandConfigPath, UnsetHomeBehindLeadingTildeIsError) {
    ASSERT_EQ(::unsetenv("HOME"), 0);
    auto r = expandConfigPath("~/x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("HOME"), std::string::npos);
    ::setenv("HOME", "/home/tester", 1); // restore for other tests
}

TEST(ExpandConfigPath, ExpandsEnvironmentVariable) {
    ASSERT_EQ(::setenv("AID_TEST_ROOT", "/opt/aid-root", 1), 0);
    auto r = expandConfigPath("${AID_TEST_ROOT}/var/log/backend.log");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "/opt/aid-root/var/log/backend.log");
}

TEST(ExpandConfigPath, ExpandsMultipleVariablesAndTilde) {
    ASSERT_EQ(::setenv("HOME", "/home/tester", 1), 0);
    ASSERT_EQ(::setenv("AID_SUB", "plugins", 1), 0);
    auto r = expandConfigPath("~/aid-dev/${AID_SUB}/openproject_plugin.so");
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "/home/tester/aid-dev/plugins/openproject_plugin.so");
}

TEST(ExpandConfigPath, UnsetVariableIsError) {
    ::unsetenv("AID_DEFINITELY_UNSET");
    auto r = expandConfigPath("${AID_DEFINITELY_UNSET}/x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("unset"), std::string::npos);
}

TEST(ExpandConfigPath, MalformedReferenceMissingBraceIsError) {
    auto r = expandConfigPath("/x/${AID_TEST_ROOT/y");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("malformed"), std::string::npos);
}

TEST(ExpandConfigPath, InvalidVariableNameIsError) {
    auto r = expandConfigPath("${1BAD}/x");
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("invalid variable name"), std::string::npos);
}

// Accessor-level: a ${VAR} in a real config path is expanded by logger().
TEST(Config, LoggerExpandsVariableInLogPath) {
    ASSERT_EQ(::setenv("AID_TEST_ROOT", "/opt/aid-root", 1), 0);
    constexpr std::string_view body = R"({
        "Logger": {
            "level": "INFO",
            "backendLogPath": "${AID_TEST_ROOT}/var/log/backend.log",
            "frontendLogPath": "~/frontend.log"
        }
    })";
    ASSERT_EQ(::setenv("HOME", "/home/tester", 1), 0);
    auto cf = makeConfigFile(body, 0640);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value()) << cfg.error().message;

    auto lg = cfg->logger();
    ASSERT_TRUE(lg.has_value()) << lg.error().message;
    EXPECT_EQ(lg->backendLogPath, "/opt/aid-root/var/log/backend.log");
    EXPECT_EQ(lg->frontendLogPath, "/home/tester/frontend.log");
}
