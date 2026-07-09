#include <trantor/utils/Logger.h>

#include <cstdint>
#include <string_view>

#include "aid/crosscutting/Logger.h"

// TODO(trantor-bridge-test): cover end-to-end via the future bootstrap
// integration test — a unit test would have
// to spin up trantor's logger machinery just to observe one callback hop.

namespace aid::crosscutting {

namespace {

// Trantor's output callback is `(const char*, uint64_t)` — the level isn't
// passed through. The level token IS embedded in the formatted line ("...
// INFO ...", "... ERROR ..." etc.), so we recover it by substring scan.
// Probed in descending severity so a message body containing "INFO" inside
// an ERROR line still classifies as ERROR.
LogLevel inferTrantorLevel(std::string_view sv) noexcept {
    if (sv.find("FATAL") != std::string_view::npos)
        return LogLevel::FATAL;
    if (sv.find("ERROR") != std::string_view::npos)
        return LogLevel::ERROR;
    if (sv.find("WARN") != std::string_view::npos)
        return LogLevel::WARN;
    if (sv.find("INFO") != std::string_view::npos)
        return LogLevel::INFO;
    if (sv.find("DEBUG") != std::string_view::npos)
        return LogLevel::DEBUG;
    if (sv.find("TRACE") != std::string_view::npos)
        return LogLevel::TRACE;
    return LogLevel::INFO;
}

} // namespace

void Logger::routeTrantor() {
    trantor::Logger::setOutputFunction(
        [](const char* msg, std::uint64_t len) noexcept {
            // Trantor's callback is invoked from trantor-internal threads.
            // An exception unwinding into trantor's frame would terminate the
            // process — same hazard as detached coroutines.
            // Swallow everything; logger failure must not crash.
            try {
                std::string_view sv{msg, static_cast<std::size_t>(len)};
                while (!sv.empty() && (sv.back() == '\n' || sv.back() == '\r')) {
                    sv.remove_suffix(1);
                }
                Logger::instance().log(inferTrantorLevel(sv), LogType::BACKEND, sv);
            } catch (...) {
                // Intentionally silent — we have no usable log sink to report to.
            }
        },
        []() noexcept { /* flush hook: LogSink flushes on every write. */ });

    // Trantor's log threshold should not gate ours — let our LogSink/level
    // gate the volume. Set trantor to TRACE so it always emits to the
    // callback; our Logger::log() filters by Logger::level_.
    trantor::Logger::setLogLevel(trantor::Logger::kTrace);
}

} // namespace aid::crosscutting
