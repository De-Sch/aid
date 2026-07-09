#pragma once

#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

namespace aid::fakes {

// Records every call and returns canned responses from per-method deques.
// A test stubs the deque it cares about; unstubbed methods return
// Error{InvariantViolation,"FakeTicketStore: no canned response for <name>"}
// so a buggy use case can't silently get default-constructed Tickets.
class FakeTicketStore final : public aid::ports::TicketStore {
public:
    std::vector<aid::TicketId> fetchById_args;
    std::vector<aid::CallId> findByExactCallid_args;
    std::vector<aid::CallId> findByCallidContains_args;
    std::vector<aid::ProjectId> findLatestOpenCallInProject_args;
    std::vector<std::pair<aid::ProjectId, std::string>> findOpenInProjectBySubject_args;
    std::vector<std::pair<aid::ProjectId, aid::PhoneNumber>> findOpenInProjectByCallerNumber_args;
    std::vector<std::string> resolveUser_args;
    std::vector<aid::UserHandle> listProjectsForUser_args;
    std::vector<aid::UserHandle> listDashboard_args;
    std::vector<std::pair<aid::Ticket, aid::UserHandle>> buildEntry_args;
    std::vector<aid::NewTicket> created;
    // `save` is reducer-based (TicketStore::save(id, reduce)). The test stubs
    // `nextSave` with the SEED ticket the adapter would have fetched (the fresh
    // server state); the fake applies the caller's reducer to it and records the
    // POST-reducer result here, so assertions on `saved[0]` see the same fields
    // the use case used to set inline. `save_args` records the ids saved.
    std::vector<aid::TicketId> save_args;
    std::vector<aid::Ticket> saved;
    std::vector<std::pair<aid::TicketId, aid::UserHandle>> addCallHandler_args;
    std::vector<aid::Ticket> recipientsFor_args;
    std::vector<aid::ProjectId> openCallsInProject_args;
    int refreshMembership_calls = 0;
    std::vector<aid::TicketId> closed;
    std::vector<std::string> decodeWebhook_args;

    std::deque<aid::plumbing::Result<aid::Ticket>> nextFetchById;
    std::deque<aid::plumbing::Result<std::optional<aid::Ticket>>> nextFindByExactCallid;
    std::deque<aid::plumbing::Result<std::optional<aid::Ticket>>> nextFindByCallidContains;
    std::deque<aid::plumbing::Result<std::optional<aid::Ticket>>> nextFindLatestOpenCallInProject;
    std::deque<aid::plumbing::Result<std::optional<aid::Ticket>>> nextFindOpenInProjectBySubject;
    std::deque<aid::plumbing::Result<std::optional<aid::Ticket>>>
        nextFindOpenInProjectByCallerNumber;
    std::deque<aid::plumbing::Result<std::optional<aid::UserHandle>>> nextResolveUser;
    std::deque<aid::plumbing::Result<std::vector<aid::ProjectId>>> nextListProjectsForUser;
    std::deque<aid::plumbing::Result<std::vector<aid::DashboardEntry>>> nextListDashboard;
    std::deque<aid::plumbing::Result<aid::TicketId>> nextCreate;
    std::deque<aid::plumbing::Result<aid::Ticket>> nextSave;
    std::deque<aid::plumbing::Result<void>> nextAddCallHandler;
    std::deque<aid::plumbing::Result<std::vector<aid::UserHandle>>> nextRecipientsFor;
    std::deque<aid::plumbing::Result<std::vector<aid::Ticket>>> nextOpenCallsInProject;
    std::deque<aid::plumbing::Result<std::vector<aid::MembershipDelta>>> nextRefreshMembership;
    std::deque<aid::plumbing::Result<void>> nextClose;
    std::deque<aid::plumbing::Result<std::optional<aid::WebhookDecode>>> nextDecodeWebhook;
    std::deque<aid::plumbing::Result<void>> nextPing;

    int ping_calls = 0;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    fetchById(aid::TicketId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByExactCallid(aid::CallId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByCallidContains(aid::CallId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findLatestOpenCallInProject(aid::ProjectId project) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectByCallerNumber(aid::ProjectId project, aid::PhoneNumber caller) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
    resolveUser(std::string_view login) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::ProjectId>>>
    listProjectsForUser(aid::UserHandle user) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::DashboardEntry>>>
    listDashboard(aid::UserHandle viewer) override;

    [[nodiscard]] aid::DashboardEntry buildEntry(const aid::Ticket& ticket,
                                                 aid::UserHandle viewer) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::TicketId>>
    create(const aid::NewTicket& ticket) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    save(aid::TicketId id, aid::ports::TicketReducer reduce) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    addCallHandler(aid::TicketId id, aid::UserHandle login) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
    recipientsFor(const aid::Ticket& ticket) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    openCallsInProject(aid::ProjectId project) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::MembershipDelta>>>
    refreshMembership() override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> close(aid::TicketId id) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::WebhookDecode>>>
    decodeWebhook(std::string payload) override;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> ping() override;
};

} // namespace aid::fakes
