#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/adapters/openproject/internal/CustomFieldMap.h"
#include "aid/adapters/openproject/internal/OpHttp.h"
#include "aid/adapters/openproject/internal/OpStatusMap.h"
#include "aid/adapters/openproject/internal/OpUserRepo.h"
#include "aid/crosscutting/Config.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/ports/TicketStore.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

// OpTicketRepo — every TicketStore ticket-CRUD method, plus the
// closeTwoStep walk. This is the single point of OpenProject HAL contact
// for work_packages: every find* uses parseFromHal; every PATCH goes
// through OpHttp::retryOn409.
//
// Filter-JSON discipline (injection defence): all caller-supplied values
// flow through filter(), which builds via nlohmann::json (escapes quotes/
// backslashes) and URL-encodes. Hand-stitched filter strings are forbidden.

namespace aid::adapters::openproject {

class ProducedLedger;
class HandlerLedger;

class OpTicketRepo {
public:
    // `producedLedger` (Phase 6) records the lockVersion every create()/save()
    // writes, so the adapter's decodeWebhook can suppress self-induced echoes.
    // `handlerLedger` (Phase 6 / S7) records the callHandler login SET on every
    // fetch/create/save/addCallHandler, so decodeWebhook can diff an admin's
    // handler-drop edit against the last-known set. Both are optional (nullptr) —
    // the repo's own tests construct it without one and simply skip the
    // bookkeeping; the plugin always wires both in.
    OpTicketRepo(OpHttp& http, OpUserRepo& users, const OpStatusMap& statusMap,
                 const aid::crosscutting::TicketSystemConfig& cfg, const CustomFieldMap& fieldMap,
                 ProducedLedger* producedLedger = nullptr, HandlerLedger* handlerLedger = nullptr);

    OpTicketRepo(const OpTicketRepo&) = delete;
    OpTicketRepo& operator=(const OpTicketRepo&) = delete;
    OpTicketRepo(OpTicketRepo&&) = delete;
    OpTicketRepo& operator=(OpTicketRepo&&) = delete;
    ~OpTicketRepo() = default;

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    fetchById(aid::TicketId id);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByExactCallid(aid::CallId callid);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findByCallidContains(aid::CallId callid);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findLatestOpenCallInProject(aid::ProjectId project);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<aid::Ticket>>>
    findOpenInProjectByCallerNumber(aid::ProjectId project, aid::PhoneNumber caller);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::TicketId>>
    create(const aid::NewTicket& nt);

    // Persist a mutation to ticket `id` expressed as a pure delta. Mirrors
    // addCallHandler's refetch→apply→patch loop, generalised to any field:
    // fetch the current ticket, apply `reduce` to that FRESH state, PATCH the
    // result through the shared 409 loop, and on a conflict re-fetch + re-apply
    // `reduce` so a concurrent same-ticket writer's delta-field edit
    // (callLength / callId / description) is never clobbered.
    // `reduce` MUST be pure + total (see ports/TicketStore.h). Absolute-target
    // fields (status/assignee/...) it sets are last-writer-wins by intent.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::Ticket>>
    save(aid::TicketId id, aid::ports::TicketReducer reduce);

    // Record `login` in the ticket's callHandler CSV (customField7), appending
    // only if absent (dedup). OpenProject allows a single assignee, so each
    // accept/transfer/outgoing handler that is NOT the assignee would otherwise
    // be invisible; the CSV is the visibility mechanism (later dashboard phases
    // read it). The merge is concurrency-safe: a dedicated refetch→union→patch
    // loop re-reads the freshest callHandlers and re-unions `login` on every 409
    // retry, so two mailboxes recording different handlers on the same ticket
    // both survive instead of clobbering each other. The PATCH body is minimal
    // (lockVersion + callHandler only), so it never overwrites other fields.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    addCallHandler(aid::TicketId id, aid::UserHandle login);

    // The two-step status walk. Idempotent on
    // already-closed tickets (path() returns []).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> closeTwoStep(aid::TicketId id);

    // Used by OpDashboardBuilder (commit 3) — filter by project list +
    // type=Call + status in {New, InProgress}.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    findCallTicketsInProjectsOpen(const std::vector<aid::ProjectId>& projects);

    // The open (New + InProgress) call tickets in a SINGLE project. Filters
    // project=X + type=Call + status in {New, InProgress}, routed through
    // getAllPaged so a project with >20 open call tickets is not silently
    // truncated by OpenProject's default page size (see the dashboard
    // pagination fix / getAllPaged). This is the per-project listing the
    // membership reconciler runs ONLY when a project's member set actually
    // changed — it holds no membership logic itself (the diff that decides
    // whether to call it lives in the reconciler use case).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    openCallsInProject(aid::ProjectId project);

    // Used by OpDashboardBuilder — tickets assigned to a user (regardless
    // of type), excluding Closed. Filter is on the assignee href.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    findOpenTicketsAssignedTo(aid::UserHandle user);

    // Used by OpDashboardBuilder — open call tickets where `viewer` is recorded
    // as a call handler, REGARDLESS of project membership. This is the
    // cross-project visibility arm: a user who accepted/made/transferred a call
    // in a project they are not a member of must still see that ticket. Filters
    // type=Call + status in {New, InProgress} + callHandler (customField7) ~
    // viewer.login (operator "~", the only contains operator OpenProject offers
    // on a text custom field). Because "~" is a substring match it can also
    // match "alice2"/"malice", so the parsed result is post-filtered to EXACT
    // membership in the callHandler CSV — keeping this predicate identical to
    // recipientsFor (same rule, inverted) so the dashboard shows no ghost rows.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    findCallTicketsWithHandler(aid::UserHandle viewer);

    // The set of users who should see `t`: the members of its project UNION the
    // logins recorded in its callHandler CSV, deduped by login. This is the
    // exact inverse of the dashboard's two visibility arms
    // (findCallTicketsInProjectsOpen ∪ findCallTicketsWithHandler), so a ticket
    // appears on a viewer's dashboard iff that viewer is in recipientsFor(t).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
    recipientsFor(const aid::Ticket& t);

    // The recipients who LOST visibility of `t` as a result of the webhook that
    // produced it — the logins an admin removed from the callHandler CSV who are
    // NOT project members (a still-member loses nothing: the membership arm keeps
    // the ticket on their board). Reads the prior handler set out of (and installs
    // the new one into) the HandlerLedger, diffs prior \ new, and refetches THIS
    // project's membership only when that diff is non-empty — so the common edit
    // with no handler dropped costs no extra OpenProject query. Cold start (no
    // prior set) or no ledger ⇒ empty. The membership fetch is the sole error
    // source; the adapter treats a failure as "no drops surfaced this round"
    // (best effort — the REST dashboard self-heals on the recipient's next load).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::UserHandle>>>
    droppedRecipientsOnWebhook(const aid::Ticket& t);

private:
    // Map a (possibly absent) assignee to the href that toCreate/PatchPayload
    // should emit, or nullopt to omit the assignee link. The value may be a
    // login (resolve via OpUserRepo::hrefFor), an already-numeric user id, or a
    // full "/api/v3/users/<id>" href — the latter two round-trip from a fetched
    // ticket (no ?include=assignee) and must NOT be re-resolved as a login.
    // When omitOnLookupFailure is true, a login that fails to resolve yields
    // nullopt (omit) instead of an error — see the definition for why this is a
    // correct no-op on the save() / hangup / close path.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::optional<std::string>>>
    resolveAssigneeHref(const std::optional<aid::UserHandle>& assignee, bool omitOnLookupFailure);

    // Fetch EVERY work_package matching an already-built `…?filters=<…>` URL,
    // paging through OpenProject's HAL collection until exhausted. OpenProject
    // caps an unparameterised list at its default page size (20) and silently
    // drops the rest, so the dashboard snapshot arms (findCallTicketsInProjects-
    // Open / findCallTicketsWithHandler / findOpenTicketsAssignedTo) MUST page
    // or they truncate. Loops offset=1,2,… appending &offset&pageSize, stopping
    // on the first short page — so a result set smaller than one page costs
    // exactly one GET. Only the snapshot arms use this; the single-ticket delta
    // path never does (it must add no extra OpenProject queries).
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::vector<aid::Ticket>>>
    getAllPaged(std::string baseUrlWithFilters);

    OpHttp& http_;
    OpUserRepo& users_;
    const OpStatusMap& statusMap_;
    const aid::crosscutting::TicketSystemConfig& cfg_;
    const CustomFieldMap& fieldMap_;
    ProducedLedger* producedLedger_;
    HandlerLedger* handlerLedger_;
};

} // namespace aid::adapters::openproject
