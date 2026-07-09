#include "aid/value-types/TimeFormat.h"

#include <chrono>
#include <ctime>

namespace aid {

std::string formatIso8601Utc(Timestamp t) {
    const std::time_t tt = std::chrono::system_clock::to_time_t(t);
    std::tm tm{};
    ::gmtime_r(&tt, &tm);
    char buf[32]{};
    const std::size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string{buf, n};
}

} // namespace aid
