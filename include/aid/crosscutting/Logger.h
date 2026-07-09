#pragma once

#include <atomic>
#include <optional>
#include <string_view>

#include "aid/crosscutting/LogSink.h"

namespace aid::crosscutting {

// Underlying type pinned so the ordering used by Logger::log's threshold
// check (static_cast<int>(level) compare) is part of the contract.
enum class LogLevel : int { TRACE = 0, DEBUG, INFO, WARN, ERROR, FATAL };
enum class LogType : int { BACKEND = 0, FRONTEND };

class Logger {
public:
    static Logger& instance();

    // Single-threaded: must be called from Main before any other thread
    // starts logging. Subsequent calls (e.g. from test setup) are allowed
    // but inherit the same constraint. Throws std::runtime_error if either
    // sink path cannot be opened.
    static void initialize(LogLevel level, std::string_view backendLogPath,
                           std::string_view frontendLogPath);

    void log(LogLevel lv, LogType ty, std::string_view msg,
             std::optional<std::string_view> cid = std::nullopt);

    void trace(std::string_view msg, LogType ty = LogType::BACKEND,
               std::optional<std::string_view> cid = std::nullopt);
    void debug(std::string_view msg, LogType ty = LogType::BACKEND,
               std::optional<std::string_view> cid = std::nullopt);
    void info(std::string_view msg, LogType ty = LogType::BACKEND,
              std::optional<std::string_view> cid = std::nullopt);
    void warn(std::string_view msg, LogType ty = LogType::BACKEND,
              std::optional<std::string_view> cid = std::nullopt);
    void error(std::string_view msg, LogType ty = LogType::BACKEND,
               std::optional<std::string_view> cid = std::nullopt);
    void fatal(std::string_view msg, LogType ty = LogType::BACKEND,
               std::optional<std::string_view> cid = std::nullopt);

    [[nodiscard]] LogLevel currentLevel() const noexcept;

    // Defined in aid_log_trantor (links Drogon). Declared here so the spec's
    // API shape is preserved; calling from code linked only against
    // aid_crosscutting is an intentional link-time error.
    static void routeTrantor();

private:
    Logger() = default;

    LogSink backend_;
    LogSink frontend_;
    std::atomic<LogLevel> level_{LogLevel::INFO};
};

} // namespace aid::crosscutting
