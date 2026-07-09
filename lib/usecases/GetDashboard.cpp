#include "aid/usecases/GetDashboard.h"

#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/plumbing/Error.h"
#include "aid/ports/AddressBook.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/Dashboard.h"

namespace aid::usecases {

using aid::plumbing::Result;
using aid::plumbing::Task;

namespace {

// Extract the project segment from a ticket-system-style href such as
// "https://op.example/projects/support/work_packages/42" → "support".
// Returns empty if the marker is missing.
[[nodiscard]] std::string projectNameFromHref(std::string_view href) {
    constexpr std::string_view marker = "/projects/";
    const auto pos = href.find(marker);
    if (pos == std::string_view::npos) {
        return std::string{};
    }
    const auto begin = pos + marker.size();
    const auto endSlash = href.find('/', begin);
    const auto end = endSlash == std::string_view::npos ? href.size() : endSlash;
    return std::string{href.substr(begin, end - begin)};
}

} // namespace

GetDashboard::GetDashboard(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab)
    : ts_(ts), ab_(ab) {
}

Task<Result<aid::DashboardView>> GetDashboard::run(aid::UserHandle viewer) {
    auto listed = co_await ts_.listDashboard(std::move(viewer));
    if (!listed.has_value()) {
        co_return aid::plumbing::unexpected{listed.error()};
    }
    auto tickets = std::move(*listed);

    std::optional<aid::ActiveCall> active;
    for (const auto& e : tickets) {
        if (e.activeCallForViewer.has_value()) {
            active = aid::ActiveCall{e.id, *e.activeCallForViewer, projectNameFromHref(e.href),
                                     e.callerNumber};
            break;
        }
    }

    // Address-book hint: when the viewer has an active call, resolve the
    // caller's Contact via the same lookup the call flow uses. Canonicalize
    // first to honour AddressBook::lookup's precondition. A lookup failure is
    // propagated; a clean "no match" simply leaves the hint absent.
    std::optional<aid::Contact> addressCallInformation;
    if (active.has_value()) {
        const aid::PhoneNumber canonical = ab_.canonicalize(active->callerNumber);
        auto contact = co_await ab_.lookup(canonical);
        if (!contact.has_value()) {
            co_return aid::plumbing::unexpected{contact.error()};
        }
        addressCallInformation = std::move(*contact);
    }

    co_return aid::DashboardView{std::move(tickets), std::move(active),
                                 std::move(addressCallInformation)};
}

} // namespace aid::usecases
