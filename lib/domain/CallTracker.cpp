#include "aid/domain/CallTracker.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace aid::domain {

namespace {

constexpr std::string_view kWhitespace = " \t\r\n";

std::string_view trim(std::string_view s) noexcept {
    const auto first = s.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = s.find_last_not_of(kWhitespace);
    return s.substr(first, last - first + 1);
}

} // namespace

std::vector<CallId> CallTracker::decode(std::string_view field) {
    std::vector<CallId> out;
    if (field.empty()) {
        return out;
    }
    std::size_t pos = 0;
    while (true) {
        const auto comma = field.find(',', pos);
        const auto end = (comma == std::string_view::npos) ? field.size() : comma;
        const auto piece = trim(field.substr(pos, end - pos));
        if (!piece.empty()) {
            out.push_back(CallId{std::string{piece}});
        }
        if (comma == std::string_view::npos) {
            break;
        }
        pos = comma + 1;
    }
    return out;
}

std::string CallTracker::encode(std::span<const CallId> ids) {
    if (ids.empty()) {
        return {};
    }
    std::string out = ids.front().v;
    for (std::size_t i = 1; i < ids.size(); ++i) {
        out += ", ";
        out += ids[i].v;
    }
    return out;
}

bool CallTracker::contains(std::string_view field, const CallId& id) {
    return field.find(id.v) != std::string_view::npos;
}

std::string CallTracker::withAdded(std::string_view field, const CallId& id) {
    if (contains(field, id)) {
        return std::string{field};
    }
    if (field.empty()) {
        return id.v;
    }
    std::string out{field};
    out += ", ";
    out += id.v;
    return out;
}

std::string CallTracker::withRemoved(std::string_view field, const CallId& id) {
    const auto ids = decode(field);
    std::vector<CallId> filtered;
    filtered.reserve(ids.size());
    for (const auto& x : ids) {
        if (!(x == id)) {
            filtered.push_back(x);
        }
    }
    return encode(filtered);
}

} // namespace aid::domain
