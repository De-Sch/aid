#include "aid/value-types/CallEvent.h"

#include <variant>

namespace aid {

namespace {

struct EventNameVisitor {
    std::string_view operator()(const IncomingCall&) const noexcept { return "Incoming Call"; }
    std::string_view operator()(const OutgoingCall&) const noexcept { return "Outgoing Call"; }
    std::string_view operator()(const AcceptedCall&) const noexcept { return "Accepted Call"; }
    std::string_view operator()(const TransferCall&) const noexcept { return "Transfer Call"; }
    std::string_view operator()(const HangupCall&) const noexcept { return "Hangup"; }
};

} // namespace

std::string_view eventName(const CallEvent& e) noexcept {
    return std::visit(EventNameVisitor{}, e);
}

CallId callidOf(const CallEvent& e) {
    return std::visit([](const auto& v) -> CallId { return v.callid; }, e);
}

} // namespace aid
