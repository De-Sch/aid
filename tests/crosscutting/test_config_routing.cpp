#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Error.h"

namespace fs = std::filesystem;

using aid::crosscutting::Config;
using aid::crosscutting::PluginsConfig;
using aid::plumbing::ErrorCode;

namespace {

struct ConfigFile {
    fs::path dir;
    fs::path path;
    ~ConfigFile() {
        std::error_code ec;
        fs::remove_all(dir, ec);
    }
};

ConfigFile makeConfigFile(std::string_view body, mode_t mode = 0640) {
    static std::atomic<std::uint64_t> counter{0};
    const auto pid = static_cast<std::uint64_t>(::getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now =
        static_cast<std::uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());

    std::ostringstream name;
    name << "aid_cfgr_" << pid << "_" << now << "_" << seq;

    ConfigFile cf;
    cf.dir = fs::temp_directory_path() / name.str();
    fs::create_directories(cf.dir);
    cf.path = cf.dir / "config.json";

    {
        std::ofstream out(cf.path);
        out << body;
    }
    EXPECT_EQ(::chmod(cf.path.c_str(), mode), 0);
    return cf;
}

} // namespace

TEST(ConfigTicketRouting, ParsesRequiredAndOptionalKeys) {
    constexpr std::string_view body = R"({
        "TicketRouting": {
            "unknownFallback": "FB",
            "incognitoSubject": "Hidden Caller"
        }
    })";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->ticketRouting();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->unknownFallback.v, "FB");
    EXPECT_EQ(r->incognitoSubject, "Hidden Caller");
}

TEST(ConfigTicketRouting, DefaultsIncognitoSubjectWhenOmitted) {
    constexpr std::string_view body = R"({
        "TicketRouting": {
            "unknownFallback": "PROJ"
        }
    })";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->ticketRouting();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->unknownFallback.v, "PROJ");
    EXPECT_EQ(r->incognitoSubject, "Incognito Caller");
}

TEST(ConfigTicketRouting, FailsWhenUnknownFallbackMissing) {
    constexpr std::string_view body = R"({"TicketRouting": {"incognitoSubject": "X"}})";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->ticketRouting();
    ASSERT_FALSE(r.has_value());
    EXPECT_EQ(r.error().code, ErrorCode::InvalidInput);
    EXPECT_NE(r.error().message.find("unknownFallback"), std::string::npos);
}

TEST(ConfigTicketRouting, FailsWhenSectionMissing) {
    auto cf = makeConfigFile("{}");
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->ticketRouting();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("TicketRouting"), std::string::npos);
}

TEST(ConfigLanInterface, ReturnsTopLevelString) {
    constexpr std::string_view body = R"({"lanInterface": "0.0.0.0"})";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->lanInterface();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(*r, "0.0.0.0");
}

TEST(ConfigLanInterface, FailsWhenMissing) {
    auto cf = makeConfigFile("{}");
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->lanInterface();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("lanInterface"), std::string::npos);
}

TEST(ConfigLanInterface, FailsWhenNotAString) {
    auto cf = makeConfigFile(R"({"lanInterface": 8080})");
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->lanInterface();
    ASSERT_FALSE(r.has_value());
}

TEST(ConfigPlugins, ParsesBothLibPaths) {
    constexpr std::string_view body = R"({
        "Plugins": {
            "ticketStore": {"libPath": "/lib/aid_openproject_plugin.so"},
            "addressBook": {"libPath": "/lib/aid_davical_plugin.so"}
        }
    })";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->plugins();
    ASSERT_TRUE(r.has_value()) << r.error().message;
    EXPECT_EQ(r->ticketStoreSoPath, "/lib/aid_openproject_plugin.so");
    EXPECT_EQ(r->addressBookSoPath, "/lib/aid_davical_plugin.so");
}

TEST(ConfigPlugins, FailsWhenTicketStoreSubsectionMissing) {
    constexpr std::string_view body = R"({
        "Plugins": {"addressBook": {"libPath": "/x.so"}}
    })";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->plugins();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("ticketStore"), std::string::npos);
}

TEST(ConfigPlugins, FailsWhenLibPathMissing) {
    constexpr std::string_view body = R"({
        "Plugins": {
            "ticketStore": {},
            "addressBook": {"libPath": "/x.so"}
        }
    })";
    auto cf = makeConfigFile(body);
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->plugins();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("libPath"), std::string::npos);
}

TEST(ConfigPlugins, FailsWhenSectionMissing) {
    auto cf = makeConfigFile("{}");
    auto cfg = Config::load(cf.path.string());
    ASSERT_TRUE(cfg.has_value());

    auto r = cfg->plugins();
    ASSERT_FALSE(r.has_value());
    EXPECT_NE(r.error().message.find("Plugins"), std::string::npos);
}
