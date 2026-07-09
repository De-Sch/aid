#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <string>

namespace aid {

template <class Tag> struct Id {
    std::string v;

    [[nodiscard]] bool empty() const noexcept { return v.empty(); }

    friend bool operator==(const Id& a, const Id& b) noexcept { return a.v == b.v; }
    friend bool operator<(const Id& a, const Id& b) noexcept { return a.v < b.v; }
};

struct CallIdTag;
using CallId = Id<CallIdTag>;
struct TicketIdTag;
using TicketId = Id<TicketIdTag>;
struct UserHandleTag;
using UserHandle = Id<UserHandleTag>;
struct ProjectIdTag;
using ProjectId = Id<ProjectIdTag>;
struct StatusIdTag;
using StatusId = Id<StatusIdTag>;
struct CustomFieldIdTag;
using CustomFieldId = Id<CustomFieldIdTag>;

struct PhoneNumber {
    std::string v;

    [[nodiscard]] bool empty() const noexcept { return v.empty(); }

    friend bool operator==(const PhoneNumber& a, const PhoneNumber& b) noexcept {
        return a.v == b.v;
    }
    friend bool operator<(const PhoneNumber& a, const PhoneNumber& b) noexcept { return a.v < b.v; }
};

using Timestamp = std::chrono::system_clock::time_point;

enum class TicketStatus { New, InProgress, Closed };

} // namespace aid

namespace std {

template <class Tag> struct hash<aid::Id<Tag>> {
    std::size_t operator()(const aid::Id<Tag>& id) const noexcept {
        return std::hash<std::string>{}(id.v);
    }
};

template <> struct hash<aid::PhoneNumber> {
    std::size_t operator()(const aid::PhoneNumber& p) const noexcept {
        return std::hash<std::string>{}(p.v);
    }
};

} // namespace std
