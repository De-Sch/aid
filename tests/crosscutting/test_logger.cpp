#include <gtest/gtest.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>

#include "aid/crosscutting/Logger.h"

namespace fs = std::filesystem;

namespace {

struct Paths {
    fs::path backend;
    fs::path frontend;
};

Paths uniquePaths(std::string_view stem) {
    static std::atomic<uint64_t> counter{0};
    const auto pid = static_cast<uint64_t>(::getpid());
    const auto seq = counter.fetch_add(1, std::memory_order_relaxed);
    const auto now =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ostringstream name;
    name << stem << "_" << pid << "_" << now << "_" << seq;
    auto base = fs::temp_directory_path() / name.str();
    fs::create_directories(base);
    return Paths{base / "backend.log", base / "frontend.log"};
}

std::string readWhole(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

} // namespace

using aid::crosscutting::Logger;
using aid::crosscutting::LogLevel;
using aid::crosscutting::LogType;

TEST(Logger, InstanceReturnsSameSingleton) {
    auto& a = Logger::instance();
    auto& b = Logger::instance();
    EXPECT_EQ(&a, &b);
}

TEST(Logger, InitializeOpensBothSinksAndCurrentLevelMatches) {
    auto p = uniquePaths("logger_init");
    Logger::initialize(LogLevel::INFO, p.backend.string(), p.frontend.string());
    EXPECT_EQ(Logger::instance().currentLevel(), LogLevel::INFO);

    Logger::instance().info("backend-payload", LogType::BACKEND);
    Logger::instance().info("frontend-payload", LogType::FRONTEND);

    EXPECT_NE(readWhole(p.backend).find("backend-payload"), std::string::npos);
    EXPECT_NE(readWhole(p.frontend).find("frontend-payload"), std::string::npos);
}

TEST(Logger, LogLineHasFormatTimestampLevelCidMsg) {
    auto p = uniquePaths("logger_format_cid");
    Logger::initialize(LogLevel::INFO, p.backend.string(), p.frontend.string());
    Logger::instance().info("payload", LogType::BACKEND, std::optional<std::string_view>{"abc"});

    auto contents = readWhole(p.backend);
    // Find the last line containing the payload.
    std::istringstream iss(contents);
    std::string line;
    std::string match;
    while (std::getline(iss, line)) {
        if (line.find("payload") != std::string::npos)
            match = line;
    }
    ASSERT_FALSE(match.empty());
    std::regex re(R"(^\[[^\]]+\] \[INFO\] \[cid=abc\] payload$)");
    EXPECT_TRUE(std::regex_match(match, re)) << "line was: " << match;
}

TEST(Logger, LogLineOmitsCidWhenNullopt) {
    auto p = uniquePaths("logger_format_nocid");
    Logger::initialize(LogLevel::INFO, p.backend.string(), p.frontend.string());
    Logger::instance().info("payload-no-cid", LogType::BACKEND);

    auto contents = readWhole(p.backend);
    std::istringstream iss(contents);
    std::string line;
    std::string match;
    while (std::getline(iss, line)) {
        if (line.find("payload-no-cid") != std::string::npos)
            match = line;
    }
    ASSERT_FALSE(match.empty());
    std::regex re(R"(^\[[^\]]+\] \[INFO\] payload-no-cid$)");
    EXPECT_TRUE(std::regex_match(match, re)) << "line was: " << match;
    EXPECT_EQ(match.find("[cid="), std::string::npos);
}

TEST(Logger, BackendVsFrontendRoutedToCorrectSink) {
    auto p = uniquePaths("logger_routing");
    Logger::initialize(LogLevel::INFO, p.backend.string(), p.frontend.string());

    Logger::instance().info("only-backend", LogType::BACKEND);
    Logger::instance().info("only-frontend", LogType::FRONTEND);

    auto b = readWhole(p.backend);
    auto f = readWhole(p.frontend);

    EXPECT_NE(b.find("only-backend"), std::string::npos);
    EXPECT_EQ(b.find("only-frontend"), std::string::npos);

    EXPECT_NE(f.find("only-frontend"), std::string::npos);
    EXPECT_EQ(f.find("only-backend"), std::string::npos);
}

TEST(Logger, BelowThresholdLevelIsDropped) {
    auto p = uniquePaths("logger_threshold");
    Logger::initialize(LogLevel::WARN, p.backend.string(), p.frontend.string());

    Logger::instance().debug("debug-msg");
    Logger::instance().info("info-msg");
    Logger::instance().warn("warn-msg");

    auto b = readWhole(p.backend);
    EXPECT_EQ(b.find("debug-msg"), std::string::npos);
    EXPECT_EQ(b.find("info-msg"), std::string::npos);
    EXPECT_NE(b.find("warn-msg"), std::string::npos);
}

TEST(Logger, ErrorAndFatalPassThroughHighThreshold) {
    auto p = uniquePaths("logger_high");
    Logger::initialize(LogLevel::FATAL, p.backend.string(), p.frontend.string());

    Logger::instance().warn("warn-x");
    Logger::instance().error("error-x");
    Logger::instance().fatal("fatal-x");

    auto b = readWhole(p.backend);
    EXPECT_EQ(b.find("warn-x"), std::string::npos);
    EXPECT_EQ(b.find("error-x"), std::string::npos);
    EXPECT_NE(b.find("fatal-x"), std::string::npos);
}

TEST(Logger, ReinitializeRedirectsToNewPaths) {
    auto p1 = uniquePaths("logger_reinit_one");
    Logger::initialize(LogLevel::INFO, p1.backend.string(), p1.frontend.string());
    Logger::instance().info("first-payload", LogType::BACKEND);
    EXPECT_NE(readWhole(p1.backend).find("first-payload"), std::string::npos);

    auto p2 = uniquePaths("logger_reinit_two");
    Logger::initialize(LogLevel::INFO, p2.backend.string(), p2.frontend.string());
    Logger::instance().info("second-payload", LogType::BACKEND);

    EXPECT_NE(readWhole(p2.backend).find("second-payload"), std::string::npos);
    EXPECT_EQ(readWhole(p1.backend).find("second-payload"), std::string::npos);
}

TEST(Logger, LevelTokenInLineMatchesCallSite) {
    auto p = uniquePaths("logger_level_token");
    Logger::initialize(LogLevel::TRACE, p.backend.string(), p.frontend.string());

    Logger::instance().trace("t-msg");
    Logger::instance().debug("d-msg");
    Logger::instance().info("i-msg");
    Logger::instance().warn("w-msg");
    Logger::instance().error("e-msg");
    Logger::instance().fatal("f-msg");

    auto b = readWhole(p.backend);
    EXPECT_NE(b.find("[TRACE] t-msg"), std::string::npos);
    EXPECT_NE(b.find("[DEBUG] d-msg"), std::string::npos);
    EXPECT_NE(b.find("[INFO] i-msg"), std::string::npos);
    EXPECT_NE(b.find("[WARN] w-msg"), std::string::npos);
    EXPECT_NE(b.find("[ERROR] e-msg"), std::string::npos);
    EXPECT_NE(b.find("[FATAL] f-msg"), std::string::npos);
}
