#pragma once

#include <optional>
#include <string_view>
#include <variant>

#include "aid/value-types/Ids.h"

namespace aid {

struct IncomingCall {
    CallId callid;
    PhoneNumber remote;
    PhoneNumber dialed;
};

struct OutgoingCall {
    CallId callid;
    PhoneNumber remote;
    UserHandle user;
};

struct AcceptedCall {
    CallId callid;
    PhoneNumber remote;
    PhoneNumber dialed;
    std::optional<UserHandle> user;
};

struct TransferCall {
    CallId callid;
    UserHandle newUser;
};

struct HangupCall {
    CallId callid;
    PhoneNumber remote;
};

using CallEvent = std::variant<IncomingCall, OutgoingCall, AcceptedCall, TransferCall, HangupCall>;

[[nodiscard]] std::string_view eventName(const CallEvent& e) noexcept;
[[nodiscard]] CallId callidOf(const CallEvent& e);

} // namespace aid
