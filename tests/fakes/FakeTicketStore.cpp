#include "FakeTicketStore.h"

#include <utility>

namespace aid::fakes {

namespace {

template <class T>
aid::plumbing::Result<T> popOrUnstubbed(std::deque<aid::plumbing::Result<T>>& d, const char* what) {
    if (d.empty()) {
        return aid::plumbing::unexpected{aid::plumbing::Error{
            aid::plumbing::ErrorCode::InvariantViolation,
            std::string{"FakeTicketStore: no canned response for "} + what, std::nullopt}};
    }
    auto v = std::move(d.front());
    d.pop_front();
    return v;
}

} // namespace

aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
FakeTicketStore::fetchById(aid::TicketId id) {
    fetchById_args.push_back(id);
    co_return popOrUnstubbed(nextFetchById, "fetchById");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
FakeTicketStore::findByExactCallid(aid::CallId id) {
    findByExactCallid_args.push_back(id);
    co_return popOrUnstubbed(nextFindByExactCallid, "findByExactCallid");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
FakeTicketStore::findByCallidContains(aid::CallId id) {
    findByCallidContains_args.push_back(id);
    co_return popOrUnstubbed(nextFindByCallidContains, "findByCallidContains");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
FakeTicketStore::findLatestOpenCallInProject(aid::ProjectId project) {
    findLatestOpenCallInProject_args.push_back(project);
    co_return popOrUnstubbed(nextFindLatestOpenCallInProject, "findLatestOpenCallInProject");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
FakeTicketStore::findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject) {
    findOpenInProjectBySubject_args.emplace_back(std::move(project), std::string{subject});
    co_return popOrUnstubbed(nextFindOpenInProjectBySubject, "findOpenInProjectBySubject");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
FakeTicketStore::findOpenInProjectByCallerNumber(aid::ProjectId project, aid::PhoneNumber caller) {
    findOpenInProjectByCallerNumber_args.emplace_back(std::move(project), std::move(caller));
    co_return popOrUnstubbed(nextFindOpenInProjectByCallerNumber,
                             "findOpenInProjectByCallerNumber");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::UserHandle>>>
FakeTicketStore::resolveUser(std::string_view login) {
    resolveUser_args.emplace_back(login);
    co_return popOrUnstubbed(nextResolveUser, "resolveUser");
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::ProjectId>>>
FakeTicketStore::listProjectsForUser(aid::UserHandle user) {
    listProjectsForUser_args.push_back(user);
    co_return popOrUnstubbed(nextListProjectsForUser, "listProjectsForUser");
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::DashboardEntry>>>
FakeTicketStore::listDashboard(aid::UserHandle viewer) {
    listDashboard_args.push_back(viewer);
    co_return popOrUnstubbed(nextListDashboard, "listDashboard");
}

aid::DashboardEntry FakeTicketStore::buildEntry(const aid::Ticket& ticket, aid::UserHandle viewer) {
    buildEntry_args.emplace_back(ticket, std::move(viewer));
    // Deterministic projection of the fields the delta path cares about, so a
    // test can assert which ticket (and which lockVersion) reached the upsert.
    aid::DashboardEntry e;
    e.id = ticket.id;
    e.subject = ticket.subject;
    e.status = ticket.status;
    e.statusId = ticket.statusId;
    e.callIds = ticket.callIds;
    e.callerNumber = ticket.callerNumber;
    e.calledNumber = ticket.calledNumber;
    e.assignee = ticket.assignee;
    e.callStart = ticket.callStart;
    e.callEnd = ticket.callEnd;
    e.description = ticket.description;
    e.lockVersion = ticket.lockVersion;
    e.updatedAt = ticket.updatedAt;
    return e;
}

aid::plumbing::Task<aid::plumbing::Result<aid::TicketId>>
FakeTicketStore::create(const aid::NewTicket& ticket) {
    created.push_back(ticket);
    co_return popOrUnstubbed(nextCreate, "create");
}

aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
FakeTicketStore::save(aid::TicketId id, aid::ports::TicketReducer reduce) {
    save_args.push_back(id);
    // Model the adapter: apply the caller's pure reducer to the freshly fetched
    // server state (the SEED a test stubs in `nextSave`), and return the result.
    // A stubbed error short-circuits before the reducer runs (e.g. injected 409).
    auto seed = popOrUnstubbed(nextSave, "save");
    if (!seed) {
        co_return aid::plumbing::unexpected{seed.error()};
    }
    aid::Ticket reduced = reduce(std::move(*seed));
    saved.push_back(reduced);
    co_return reduced;
}

aid::plumbing::Task<aid::plumbing::Result<void>>
FakeTicketStore::addCallHandler(aid::TicketId id, aid::UserHandle login) {
    addCallHandler_args.emplace_back(std::move(id), std::move(login));
    co_return popOrUnstubbed(nextAddCallHandler, "addCallHandler");
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
FakeTicketStore::recipientsFor(const aid::Ticket& ticket) {
    recipientsFor_args.push_back(ticket);
    co_return popOrUnstubbed(nextRecipientsFor, "recipientsFor");
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
FakeTicketStore::openCallsInProject(aid::ProjectId project) {
    openCallsInProject_args.push_back(std::move(project));
    co_return popOrUnstubbed(nextOpenCallsInProject, "openCallsInProject");
}

aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::MembershipDelta>>>
FakeTicketStore::refreshMembership() {
    ++refreshMembership_calls;
    co_return popOrUnstubbed(nextRefreshMembership, "refreshMembership");
}

aid::plumbing::Task<aid::plumbing::Result<void>> FakeTicketStore::close(aid::TicketId id) {
    closed.push_back(id);
    co_return popOrUnstubbed(nextClose, "close");
}

aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::WebhookDecode>>>
FakeTicketStore::decodeWebhook(std::string payload) {
    decodeWebhook_args.push_back(std::move(payload));
    co_return popOrUnstubbed(nextDecodeWebhook, "decodeWebhook");
}

aid::plumbing::Task<aid::plumbing::Result<void>> FakeTicketStore::ping() {
    ++ping_calls;
    co_return popOrUnstubbed(nextPing, "ping");
}

} // namespace aid::fakes
