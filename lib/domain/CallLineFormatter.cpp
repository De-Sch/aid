#include "aid/domain/CallLineFormatter.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <ctime>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace aid::domain {

namespace {

// Local-time rendering at "YYYY-MM-DD HH:MM:SS" — the TZ is fixed to
// Europe/Berlin daemon-wide.
std::string formatLocalTimestamp(const Timestamp& ts) {
    const auto t = std::chrono::system_clock::to_time_t(ts);
    std::tm tm{};
    ::localtime_r(&t, &tm);
    char buf[20]; // "YYYY-MM-DD HH:MM:SS" + NUL
    const auto n = std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S", &tm);
    return std::string{buf, n};
}

// Inverse of formatLocalTimestamp. tm_isdst = -1 lets mktime consult the
// local TZ database so the parsed instant matches the rendered local time.
std::optional<Timestamp> parseLocalTimestamp(std::string_view s) {
    std::tm tm{};
    std::istringstream iss{std::string{s}};
    iss >> std::get_time(&tm, "%Y-%m-%d %H:%M:%S");
    if (iss.fail()) {
        return std::nullopt;
    }
    tm.tm_isdst = -1;
    const auto t = std::mktime(&tm);
    if (t == static_cast<std::time_t>(-1)) {
        return std::nullopt;
    }
    return std::chrono::system_clock::from_time_t(t);
}

} // namespace

std::string CallLineFormatter::buildStart(const UserHandle& user, const Timestamp& start,
                                          const CallId& callid) {
    std::string out;
    out.reserve(user.v.size() + CALL_START_PREFIX.size() + 19 + 3 + callid.v.size());
    out += user.v;
    out += CALL_START_PREFIX;
    out += formatLocalTimestamp(start);
    out += " (";
    out += callid.v;
    out += ")";
    return out;
}

std::optional<CallLineFormatter::LineSpan>
CallLineFormatter::findLineFor(std::string_view description, const CallId& callid) {
    const std::string needle = "(" + callid.v + ")";
    const auto pos = description.find(needle);
    if (pos == std::string_view::npos) {
        return std::nullopt;
    }
    const auto nlBefore = description.rfind('\n', pos);
    const std::size_t begin = (nlBefore == std::string_view::npos) ? 0 : nlBefore + 1;
    const auto nlAfter = description.find('\n', pos);
    const std::size_t end = (nlAfter == std::string_view::npos) ? description.size() : nlAfter;
    return LineSpan{begin, end};
}

std::string CallLineFormatter::rewriteUser(std::string_view line, const UserHandle& newUser) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::string{line};
    }
    std::string out{newUser.v};
    out += line.substr(colon);
    return out;
}

std::string CallLineFormatter::complete(std::string_view line, const Timestamp& end) {
    std::string out{line};
    // The "(callid)" only existed to locate this line while the call was live
    // (findLineFor). The call is over, so drop the now-useless callid that
    // otherwise clutters the log for operators reading the ticket. Strip the
    // last parenthetical, mirroring findOpenLineForUser's rfind idiom. (Safe:
    // callids carry no parens, the user handle is a paren-free login, and the
    // timestamp has none; the guards leave a malformed line untouched.)
    const auto openP = out.rfind(" (");
    const auto closeP = out.rfind(')');
    if (openP != std::string::npos && closeP != std::string::npos && closeP > openP) {
        out.erase(openP, closeP - openP + 1);
    }
    out += " Call End: ";
    out += formatLocalTimestamp(end);
    return out;
}

std::optional<CallLineFormatter::ParsedStart> CallLineFormatter::parseStart(std::string_view line) {
    const auto colon = line.find(':');
    if (colon == std::string_view::npos) {
        return std::nullopt;
    }
    const auto user = line.substr(0, colon);
    constexpr std::string_view mk = " Call start: ";
    const auto p = line.find(mk);
    if (p == std::string_view::npos) {
        return std::nullopt;
    }
    const auto tsBegin = p + mk.size();
    const auto end = line.find(" (", tsBegin);
    if (end == std::string_view::npos) {
        return std::nullopt;
    }
    const auto tsView = line.substr(tsBegin, end - tsBegin);
    auto t = parseLocalTimestamp(tsView);
    if (!t) {
        return std::nullopt;
    }
    return ParsedStart{UserHandle{std::string{user}}, *t};
}

std::optional<CallId> CallLineFormatter::findOpenLineForUser(std::string_view description,
                                                             const UserHandle& user) {
    // Supersedes the old `rfind` "most-recent-line-wins" idiom, which inspected
    // only the LAST matching line, so on a multi-call ticket where the user had an
    // earlier still-open call and a later already-hung-up one, rfind landed on
    // the closed line and reported "no active call." We now scan EVERY line —
    // mirroring findUsersWithOpenCalls — and return the callid of the FIRST
    // open line for this user (a "Call start:" line with no "Call End:"). The
    // two functions must agree on whether a user holds an open call. The
    // paren-delimited "(callid)" extraction is unchanged.
    const std::string needle = user.v + ": Call start:";
    std::size_t pos = 0;
    while (pos <= description.size()) {
        const auto eol = description.find('\n', pos);
        const std::size_t end = (eol == std::string_view::npos) ? description.size() : eol;
        const auto line = description.substr(pos, end - pos);
        if (line.find(needle) != std::string_view::npos &&
            line.find("Call End:") == std::string_view::npos) {
            const auto openP = line.rfind('(');
            const auto closeP = line.rfind(')');
            if (openP != std::string_view::npos && closeP != std::string_view::npos &&
                closeP > openP) {
                return CallId{std::string{line.substr(openP + 1, closeP - openP - 1)}};
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return std::nullopt;
}

std::vector<UserHandle> CallLineFormatter::findUsersWithOpenCalls(std::string_view description) {
    std::vector<UserHandle> users;
    std::size_t pos = 0;
    while (pos <= description.size()) {
        auto eol = description.find('\n', pos);
        const std::size_t end = (eol == std::string_view::npos) ? description.size() : eol;
        const auto line = description.substr(pos, end - pos);
        // An open line carries "Call start:" but not the "Call End:" suffix
        // a completed line gets in complete().
        if (line.find(CALL_START_PATTERN) != std::string_view::npos &&
            line.find("Call End:") == std::string_view::npos) {
            if (auto parsed = parseStart(line)) {
                const bool seen = std::any_of(users.begin(), users.end(), [&](const UserHandle& u) {
                    return u.v == parsed->user.v;
                });
                if (!seen) {
                    users.push_back(std::move(parsed->user));
                }
            }
        }
        if (eol == std::string_view::npos) {
            break;
        }
        pos = end + 1;
    }
    return users;
}

bool CallLineFormatter::hasLine(std::string_view description, const UserHandle& user,
                                const CallId& callid) {
    const std::string needle1 = user.v + ": Call start: ";
    const std::string needle2 = "(" + callid.v + ")";
    std::size_t p = description.find(needle1);
    while (p != std::string_view::npos) {
        const auto eol = description.find('\n', p);
        const std::size_t end = (eol == std::string_view::npos) ? description.size() : eol;
        if (description.substr(p, end - p).find(needle2) != std::string_view::npos) {
            return true;
        }
        p = description.find(needle1, end);
    }
    return false;
}

} // namespace aid::domain
