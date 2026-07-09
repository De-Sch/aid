#include "aid/crosscutting/Logger.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <string>
#include <string_view>

namespace aid::crosscutting {

namespace {

constexpr std::string_view levelName(LogLevel lv) noexcept {
    switch (lv) {
    case LogLevel::TRACE:
        return "TRACE";
    case LogLevel::DEBUG:
        return "DEBUG";
    case LogLevel::INFO:
        return "INFO";
    case LogLevel::WARN:
        return "WARN";
    case LogLevel::ERROR:
        return "ERROR";
    case LogLevel::FATAL:
        return "FATAL";
    }
    return "?";
}

std::string formatTimestamp(std::chrono::system_clock::time_point tp) {
    using namespace std::chrono;
    const auto secs = time_point_cast<seconds>(tp);
    const auto millis = duration_cast<milliseconds>(tp - secs).count();

    std::time_t t = system_clock::to_time_t(secs);
    std::tm local{};
    // localtime_r is required for thread safety.
    ::localtime_r(&t, &local);

    // Render: 2026-05-19T14:23:45.123+0200
    char buf[40];
    const std::size_t n = std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S", &local);
    if (n == 0)
        return std::string{"0000-00-00T00:00:00.000+0000"};

    char tail[16];
    // %z renders the timezone as +HHMM (or -HHMM); append after the millis.
    char tzbuf[8];
    std::strftime(tzbuf, sizeof tzbuf, "%z", &local);
    std::snprintf(tail, sizeof tail, ".%03lld%s", static_cast<long long>(millis), tzbuf);

    std::string out;
    out.reserve(n + 12);
    out.append(buf, n);
    out.append(tail);
    return out;
}

} // namespace

Logger& Logger::instance() {
    static Logger g;
    return g;
}

void Logger::initialize(LogLevel level, std::string_view backendLogPath,
                        std::string_view frontendLogPath) {
    auto& self = instance();
    // The spec calls for one-shot initialization from Main. Tests reinitialize
    // with fresh paths; ofstream::open on an already-open stream is a no-op,
    // so each sink owns a flag for that by reconstructing on a fresh path.
    if (self.backend_.path() != std::string{backendLogPath}) {
        self.backend_.open(backendLogPath);
    } else if (!self.backend_.isOpen()) {
        self.backend_.open(backendLogPath);
    }
    if (self.frontend_.path() != std::string{frontendLogPath}) {
        self.frontend_.open(frontendLogPath);
    } else if (!self.frontend_.isOpen()) {
        self.frontend_.open(frontendLogPath);
    }
    self.level_.store(level, std::memory_order_relaxed);
}

LogLevel Logger::currentLevel() const noexcept {
    return level_.load(std::memory_order_relaxed);
}

void Logger::log(LogLevel lv, LogType ty, std::string_view msg,
                 std::optional<std::string_view> cid) {
    if (static_cast<int>(lv) < static_cast<int>(level_.load(std::memory_order_relaxed))) {
        return;
    }

    const auto ts = formatTimestamp(std::chrono::system_clock::now());

    std::string line;
    line.reserve(ts.size() + msg.size() + 32 + (cid ? cid->size() + 8 : 0));
    line.push_back('[');
    line.append(ts);
    line.append("] [");
    line.append(levelName(lv));
    line.push_back(']');
    if (cid) {
        line.append(" [cid=");
        line.append(*cid);
        line.push_back(']');
    }
    line.push_back(' ');
    line.append(msg);

    auto& sink = (ty == LogType::BACKEND) ? backend_ : frontend_;
    sink.write(line);
}

void Logger::trace(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::TRACE, ty, msg, cid);
}
void Logger::debug(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::DEBUG, ty, msg, cid);
}
void Logger::info(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::INFO, ty, msg, cid);
}
void Logger::warn(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::WARN, ty, msg, cid);
}
void Logger::error(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::ERROR, ty, msg, cid);
}
void Logger::fatal(std::string_view msg, LogType ty, std::optional<std::string_view> cid) {
    log(LogLevel::FATAL, ty, msg, cid);
}

} // namespace aid::crosscutting
