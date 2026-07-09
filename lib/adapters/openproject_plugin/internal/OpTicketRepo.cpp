#include "aid/adapters/openproject/internal/OpTicketRepo.h"

#include <algorithm>
#include <cstddef>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aid/adapters/openproject/internal/HandlerLedger.h"
#include "aid/adapters/openproject/internal/ProducedLedger.h"
#include "aid/adapters/openproject/internal/payload.h"
#include "aid/adapters/openproject/internal/url.h"
#include "aid/domain/StateTransitions.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;

namespace aid::adapters::openproject {

OpTicketRepo::OpTicketRepo(OpHttp& http, OpUserRepo& users, const OpStatusMap& statusMap,
                           const aid::crosscutting::TicketSystemConfig& cfg,
                           const CustomFieldMap& fieldMap, ProducedLedger* producedLedger,
                           HandlerLedger* handlerLedger)
    : http_(http), users_(users), statusMap_(statusMap), cfg_(cfg), fieldMap_(fieldMap),
      producedLedger_(producedLedger), handlerLedger_(handlerLedger) {
}

Task<Result<aid::Ticket>> OpTicketRepo::fetchById(aid::TicketId id) {
    const std::string path = "/api/v3/work_packages/" + urlEncode(id.v);
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    auto parsed = parseFromHal(*resp, fieldMap_, statusMap_);
    // Warm the handler ledger on every read so decodeWebhook can later diff an
    // admin's handler-drop edit against the freshest set we have observed.
    if (parsed && handlerLedger_ != nullptr)
        handlerLedger_->record(parsed->id, parsed->callHandlers);
    co_return parsed;
}

namespace {

// Requested page size for the dashboard snapshot arms. Set to OpenProject's
// own server maximum (it clamps any larger request down to this), so any
// realistic per-user open-call-ticket count fits in a single GET and getAllPaged
// loops only when a result set genuinely exceeds it. Because getAllPaged
// terminates on the collection's `total` (not on this number), the value is a
// pure performance knob — even if a stricter OpenProject clamps it lower, the
// loop just pages more times and still fetches everything. (OpenProject's
// default when omitted is 20 — far too small; that was the silent-truncation
// bug this fixes.)
constexpr int kDashboardPageSize = 1000;

// True when s is non-empty and every character is an ASCII digit — i.e. an
// already-numeric OpenProject user id (e.g. "6"), not a login.
bool isAllDigits(std::string_view s) {
    return !s.empty() &&
           std::all_of(s.begin(), s.end(), [](unsigned char c) { return c >= '0' && c <= '9'; });
}

// Helper: take a HAL collection response and return its first parsed
// Ticket, or nullopt when the array is empty.
Result<std::optional<aid::Ticket>> firstFromCollection(const nlohmann::json& resp,
                                                       const CustomFieldMap& fieldMap,
                                                       const OpStatusMap& statusMap) {
    auto embIt = resp.find("_embedded");
    if (embIt == resp.end() || !embIt->is_object())
        return std::optional<aid::Ticket>{};
    auto elIt = embIt->find("elements");
    if (elIt == embIt->end() || !elIt->is_array() || elIt->empty())
        return std::optional<aid::Ticket>{};
    auto parsed = parseFromHal((*elIt)[0], fieldMap, statusMap);
    if (!parsed)
        return unexpected(parsed.error());
    return std::optional<aid::Ticket>{std::move(*parsed)};
}

Result<std::vector<aid::Ticket>> allFromCollection(const nlohmann::json& resp,
                                                   const CustomFieldMap& fieldMap,
                                                   const OpStatusMap& statusMap) {
    std::vector<aid::Ticket> out;
    auto embIt = resp.find("_embedded");
    if (embIt == resp.end() || !embIt->is_object())
        return out;
    auto elIt = embIt->find("elements");
    if (elIt == embIt->end() || !elIt->is_array())
        return out;
    out.reserve(elIt->size());
    for (const auto& e : *elIt) {
        auto parsed = parseFromHal(e, fieldMap, statusMap);
        if (!parsed)
            return unexpected(parsed.error());
        out.push_back(std::move(*parsed));
    }
    return out;
}

} // namespace

Task<Result<std::optional<aid::Ticket>>> OpTicketRepo::findByExactCallid(aid::CallId callid) {
    const std::string path =
        singleFilterUrl("/api/v3/work_packages", customFieldName(fieldMap_.callId), "=", callid.v);
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    co_return firstFromCollection(*resp, fieldMap_, statusMap_);
}

Task<Result<std::optional<aid::Ticket>>> OpTicketRepo::findByCallidContains(aid::CallId callid) {
    const std::string path =
        singleFilterUrl("/api/v3/work_packages", customFieldName(fieldMap_.callId), "~", callid.v);
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    co_return firstFromCollection(*resp, fieldMap_, statusMap_);
}

Task<Result<std::optional<aid::Ticket>>>
OpTicketRepo::findLatestOpenCallInProject(aid::ProjectId project) {
    // [{project=X}, {type=Call}, {status!=closedHref}, sort by updatedAt desc].
    // OpenProject expresses the !=Closed filter as operator="!" (the
    // "is not" operator) against the closed status href. ("<>" is NOT a
    // valid OpenProject filter operator and yields HTTP 422.)
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", {project.v}}}}});
    filters.push_back({{"type", {{"operator", "="}, {"values", {cfg_.typeCall}}}}});
    filters.push_back(
        {{"status",
          {{"operator", "!"}, {"values", {statusMap_.hrefIdFor(aid::TicketStatus::Closed).v}}}}});

    std::string path = multiFilterUrl("/api/v3/work_packages", filters);
    path.append("&sortBy=");
    path.append(urlEncode("[[\"updatedAt\",\"desc\"]]"));
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    co_return firstFromCollection(*resp, fieldMap_, statusMap_);
}

Task<Result<std::optional<aid::Ticket>>>
OpTicketRepo::findOpenInProjectBySubject(aid::ProjectId project, std::string_view subject) {
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", {project.v}}}}});
    // OpenProject's subject filter only allows the substring operators
    // ("~" / "!~"); operator "=" is rejected with HTTP 422. "~" (contains)
    // matches the exact subject we look up here (incognito roll-up subject
    // or caller name), so it serves as the open-ticket lookup.
    filters.push_back({{"subject", {{"operator", "~"}, {"values", {std::string{subject}}}}}});
    filters.push_back(
        {{"status",
          {{"operator", "!"}, {"values", {statusMap_.hrefIdFor(aid::TicketStatus::Closed).v}}}}});

    const std::string path = multiFilterUrl("/api/v3/work_packages", filters);
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    co_return firstFromCollection(*resp, fieldMap_, statusMap_);
}

Task<Result<std::optional<aid::Ticket>>>
OpTicketRepo::findOpenInProjectByCallerNumber(aid::ProjectId project, aid::PhoneNumber caller) {
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", {project.v}}}}});
    filters.push_back(
        {{customFieldName(fieldMap_.callerNumber), {{"operator", "="}, {"values", {caller.v}}}}});
    filters.push_back(
        {{"status",
          {{"operator", "!"}, {"values", {statusMap_.hrefIdFor(aid::TicketStatus::Closed).v}}}}});

    const std::string path = multiFilterUrl("/api/v3/work_packages", filters);
    auto resp = co_await http_.get(path);
    if (!resp)
        co_return unexpected(resp.error());
    co_return firstFromCollection(*resp, fieldMap_, statusMap_);
}

Task<Result<std::optional<std::string>>>
OpTicketRepo::resolveAssigneeHref(const std::optional<aid::UserHandle>& assignee,
                                  bool omitOnLookupFailure) {
    if (!assignee)
        co_return std::optional<std::string>{};

    const std::string& v = assignee->v;

    // An already-resolved href (e.g. "/api/v3/users/9") or a bare numeric user
    // id (e.g. "6") both round-trip from a ticket we fetched WITHOUT
    // ?include=assignee — parseFromHal could only recover the numeric tail (or
    // a display-name title). Neither is a login, so we must NOT feed them to
    // hrefFor (which filters by login and would match zero users). Rebuild the
    // href directly: for hangup/close this writes the assignee back unchanged,
    // a true no-op.
    if (v.rfind("/api/v3/users/", 0) == 0)
        co_return std::optional<std::string>{v};
    if (isAllDigits(v))
        co_return std::optional<std::string>{"/api/v3/users/" + v};

    // A login form (the create path, or a fetch that did embed the login).
    // hrefFor is normally a cache hit (resolveLogin in the use case warmed it).
    auto href = co_await users_.hrefFor(*assignee);
    if (href)
        co_return std::optional<std::string>{std::move(*href)};

    // Lookup failed. On create() this is a real fault (a freshly-resolved login
    // that should exist) and propagates. On save() the only non-login value we
    // can see is a display-name title carried over from a fetch — and save()
    // callers (hangup, closeTwoStep) never CHANGE the assignee. Omitting the
    // _links.assignee from a PATCH is a partial update that leaves OpenProject's
    // stored assignee intact, so dropping it is the correct no-op rather than
    // failing the whole event. The Accepted/Transfer save path sets a real login
    // with a warm hrefCache_, so it never reaches this branch in practice.
    if (omitOnLookupFailure)
        co_return std::optional<std::string>{};
    co_return unexpected(href.error());
}

Task<Result<aid::TicketId>> OpTicketRepo::create(const aid::NewTicket& nt) {
    const std::string path = "/api/v3/projects/" + urlEncode(nt.projectId.v) + "/work_packages";

    auto assigneeHref = co_await resolveAssigneeHref(nt.assignee, /*omitOnLookupFailure=*/false);
    if (!assigneeHref)
        co_return unexpected(assigneeHref.error());

    auto body = toCreatePayload(nt, fieldMap_, statusMap_, cfg_, *assigneeHref);
    auto resp = co_await http_.post(path, body);
    if (!resp)
        co_return unexpected(resp.error());

    // The POST response is a HAL representation of the new work_package.
    // We only need its id; parsing the whole body would force assignee /
    // status lookups for a value the caller doesn't read.
    auto idIt = resp->find("id");
    if (idIt == resp->end()) {
        co_return unexpected(Error{ErrorCode::Unknown,
                                   "OpTicketRepo::create: response has no top-level id",
                                   std::nullopt});
    }
    aid::TicketId newId;
    if (idIt->is_number_integer())
        newId = aid::TicketId{std::to_string(idIt->get<long long>())};
    else if (idIt->is_string())
        newId = aid::TicketId{idIt->get<std::string>()};
    else
        co_return unexpected(Error{ErrorCode::Unknown,
                                   "OpTicketRepo::create: id has unexpected type", std::nullopt});

    // Phase 6 echo suppression: remember the version OpenProject assigned the
    // new work package so the matching "created" webhook is recognised as our
    // own and not re-emitted as a live delta. The POST response carries
    // lockVersion at the top level (no extra round-trip / assignee lookup).
    if (producedLedger_ != nullptr) {
        if (auto lvIt = resp->find("lockVersion");
            lvIt != resp->end() && lvIt->is_number_integer()) {
            producedLedger_->record(newId, lvIt->get<int>());
        }
    }

    // A freshly created ticket carries no call handlers yet (NewTicket has none),
    // so seed the ledger with the empty baseline — the first handler added later
    // is a grow, never a drop.
    if (handlerLedger_ != nullptr)
        handlerLedger_->record(newId, std::vector<aid::UserHandle>{});

    co_return newId;
}

Task<Result<aid::Ticket>> OpTicketRepo::save(aid::TicketId id, aid::ports::TicketReducer reduce) {
    const std::string path = "/api/v3/work_packages/" + urlEncode(id.v);

    // Seed from the freshest server state, then apply the caller's pure delta to
    // it. Re-deriving the delta from the server's CURRENT state — here, and on
    // every 409 retry below — is what makes a concurrent same-ticket writer's
    // edit survive: the reducer never carries a stale
    // snapshot's callLength / callId / description across a conflict. This
    // mirrors addCallHandler's refetch→apply→patch loop, generalised to any
    // field.
    auto initial = co_await fetchById(id);
    if (!initial)
        co_return unexpected(initial.error());
    aid::Ticket t = reduce(std::move(*initial));

    // 422-tolerance on the assignee link. OpenProject rejects assigning a work
    // package to a user who is not a member of its project with HTTP 422. The
    // callHandler CSV — not the single assignee — is the real cross-project
    // visibility mechanism, so a rejected assignee must NOT fail the whole save.
    // `includeAssignee` is read inside patchFn; on a 422 that followed a PATCH
    // which ACTUALLY carried an assignee link we flip it off and re-run the
    // PATCH (omitting the link) so status/callLength still persist. A 422 makes
    // no server-side change, so t.lockVersion stays valid. `sentAssigneeLink`
    // records whether the last patchFn emitted a link, so a 422 unrelated to the
    // assignee (or on a ticket with none) propagates instead of triggering a
    // pointless second attempt.
    bool includeAssignee = true;
    bool sentAssigneeLink = false;

    // The retry callbacks close over the reduced ticket. patchFn resolves the
    // assignee href for the CURRENT reduced state (the reducer may set a new
    // assignee on accept/transfer; refresh() recomputes it from fresh) and
    // PATCHes the full body. refresh() re-fetches and re-applies the reducer so
    // the next patchFn() sees the freshly re-derived delta + lockVersion.
    auto patchFn = [this, &t, &path, &includeAssignee,
                    &sentAssigneeLink]() -> Task<Result<nlohmann::json>> {
        auto hrefRes = co_await resolveAssigneeHref(t.assignee, /*omitOnLookupFailure=*/true);
        if (!hrefRes)
            co_return unexpected(hrefRes.error());
        const std::optional<std::string> assigneeHref =
            includeAssignee ? std::move(*hrefRes) : std::nullopt;
        sentAssigneeLink = assigneeHref.has_value();
        const auto body = toPatchPayload(t, fieldMap_, statusMap_, assigneeHref);
        co_return co_await http_.patch(path, body);
    };
    auto refresh = [this, &t, &id, &reduce]() -> Task<Result<int>> {
        auto fresh = co_await fetchById(id);
        if (!fresh)
            co_return unexpected(fresh.error());
        // Re-derive the delta against the now-fresh server state (which already
        // carries any concurrent writer's edit), never the stale first snapshot.
        t = reduce(std::move(*fresh));
        co_return t.lockVersion;
    };

    auto patched = co_await http_.retryOn409(patchFn, refresh);
    if (!patched && includeAssignee && sentAssigneeLink &&
        patched.error().code == ErrorCode::Unprocessable422) {
        includeAssignee = false;
        patched = co_await http_.retryOn409(patchFn, refresh);
    }
    if (!patched)
        co_return unexpected(patched.error());

    auto result = parseFromHal(*patched, fieldMap_, statusMap_);
    // Phase 6 echo suppression: record the post-PATCH version so the webhook
    // OpenProject fires for this very edit is recognised as our own echo.
    if (result && producedLedger_ != nullptr) {
        producedLedger_->record(result->id, result->lockVersion);
    }
    // Keep the handler-set baseline fresh so a later admin handler-drop diffs
    // against what we just wrote, not a stale set.
    if (result && handlerLedger_ != nullptr) {
        handlerLedger_->record(result->id, result->callHandlers);
    }
    co_return result;
}

Task<Result<void>> OpTicketRepo::addCallHandler(aid::TicketId id, aid::UserHandle login) {
    const std::string path = "/api/v3/work_packages/" + urlEncode(id.v);

    auto contains = [&login](const aid::Ticket& tkt) {
        return std::any_of(tkt.callHandlers.begin(), tkt.callHandlers.end(),
                           [&login](const aid::UserHandle& h) { return h.v == login.v; });
    };

    // Seed from the freshest server state. If `login` is already recorded we
    // are done — "append only if absent" means no write at all.
    auto initial = co_await fetchById(id);
    if (!initial)
        co_return unexpected(initial.error());
    aid::Ticket fresh = std::move(*initial);
    if (contains(fresh))
        co_return Result<void>{};
    fresh.callHandlers.push_back(login);

    // Drive the PATCH through the shared 409 loop, but RE-MERGE on every retry:
    // refresh() re-reads the current callHandlers (which a racing accept may have
    // grown) and re-unions `login`, so two mailboxes recording different handlers
    // both survive. The body is minimal (lockVersion + callHandler), so it never
    // clobbers status/assignee/callLength a concurrent save just wrote.
    auto patchFn = [this, &fresh, &path]() -> Task<Result<nlohmann::json>> {
        const auto body = toCallHandlerPatch(fresh.lockVersion, fresh.callHandlers, fieldMap_);
        return http_.patch(path, body);
    };
    auto refresh = [this, &fresh, &id, &contains, &login]() -> Task<Result<int>> {
        auto reread = co_await fetchById(id);
        if (!reread)
            co_return unexpected(reread.error());
        fresh = std::move(*reread);
        if (!contains(fresh))
            fresh.callHandlers.push_back(login);
        co_return fresh.lockVersion;
    };

    auto patched = co_await http_.retryOn409(patchFn, refresh);
    if (!patched)
        co_return unexpected(patched.error());

    // Phase 6 echo suppression: record the post-PATCH version so the webhook
    // OpenProject fires for this very edit is recognised as our own echo. The
    // PATCH response is a full work_package HAL with lockVersion at the top
    // level, and `id` is already in scope — no parse of the full ticket is
    // needed (this returns void), matching create()'s lighter recording path.
    if (producedLedger_ != nullptr) {
        if (auto lvIt = patched->find("lockVersion");
            lvIt != patched->end() && lvIt->is_number_integer()) {
            producedLedger_->record(id, lvIt->get<int>());
        }
    }
    // Record the post-merge handler set (the one we just PATCHed) so the echo
    // webhook for this very write diffs to "no drop", and a later admin removal
    // diffs against the set that includes `login`.
    if (handlerLedger_ != nullptr)
        handlerLedger_->record(id, fresh.callHandlers);
    co_return Result<void>{};
}

Task<Result<void>> OpTicketRepo::closeTwoStep(aid::TicketId id) {
    // Status walk:
    //   t = co_await fetchById(id)
    //   for step in StateTransitions::path(t.status, Closed):
    //     t.status = step
    //     t = co_await save(t)               // wraps retryOn409 internally
    //   path([], Closed→Closed) → [] → no-op (idempotent)
    //
    // The status
    // sequence and step COUNT are preserved byte-for-byte — path() is unchanged,
    // and we still set status = step once per step. What moved is WHERE the
    // "status = step" assignment runs: instead of mutating a snapshot carried
    // between the two PATCHes, each step is a save(id, reducer) that re-fetches
    // the ticket and sets only status = step on that FRESH state. This stops the
    // two status PATCHes from clobbering a concurrent callLength / description
    // edit (the snapshot used to carry stale delta fields across both writes).
    auto ticket = co_await fetchById(id);
    if (!ticket)
        co_return unexpected(ticket.error());

    const auto steps =
        aid::domain::StateTransitions::path(ticket->status, aid::TicketStatus::Closed);
    for (aid::TicketStatus step : steps) {
        // Hoist the reducer to a named local: a temporary in the co_await operand
        // is double-destroyed under gcc-12's coroutine frame-lifetime bug.
        const aid::ports::TicketReducer stepReducer = [step](aid::Ticket fresh) {
            fresh.status = step;
            return fresh;
        };
        auto saved = co_await save(id, stepReducer);
        if (!saved)
            co_return unexpected(saved.error());
    }
    co_return Result<void>{};
}

Task<Result<std::vector<aid::Ticket>>> OpTicketRepo::getAllPaged(std::string baseUrlWithFilters) {
    // OpenProject API v3 paginates a HAL collection via `offset` (the 1-based
    // page NUMBER, not a row offset) and `pageSize`. We loop offset=1,2,…
    // accumulating each page's elements until the whole result set is collected.
    //
    // Termination is driven by the collection's `total` (the authoritative match
    // count OpenProject always reports), NOT by the page size we requested. That
    // distinction matters: OpenProject CLAMPS an over-large `pageSize` down to
    // its server maximum (request 1000 → 1000, request 5000 → still 1000). A
    // "stop when the page is shorter than what we asked for" rule would mistake
    // such a clamped full page for the last one and silently truncate — exactly
    // the bug class this fix exists to kill. Looping until `all.size() >= total`
    // is correct for any requested size and any server cap; a short/empty page
    // is only a fallback stop for the (real-OpenProject-never) case where `total`
    // is absent. A result set within one page still costs exactly one GET.
    // `baseUrlWithFilters` already carries `?filters=<…>`, so the pagination
    // params splice on with `&`.
    std::vector<aid::Ticket> all;
    long long total = -1;
    for (int page = 1;; ++page) {
        std::string path = baseUrlWithFilters;
        path.append("&offset=");
        path.append(std::to_string(page));
        path.append("&pageSize=");
        path.append(std::to_string(kDashboardPageSize));

        auto resp = co_await http_.get(path);
        if (!resp)
            co_return unexpected(resp.error());

        if (auto it = resp->find("total"); it != resp->end() && it->is_number())
            total = it->template get<long long>();

        auto parsed = allFromCollection(*resp, fieldMap_, statusMap_);
        if (!parsed)
            co_return unexpected(parsed.error());

        const std::size_t got = parsed->size();
        for (auto& t : *parsed)
            all.push_back(std::move(t));

        // Authoritative stop: every matching row has been collected.
        if (total >= 0 && static_cast<long long>(all.size()) >= total)
            break;
        // Fallbacks (only when `total` was never reported): an empty page means
        // nothing more to read; a short page means OpenProject honoured our full
        // request and had no more rows.
        if (got == 0)
            break;
        if (total < 0 && got < static_cast<std::size_t>(kDashboardPageSize))
            break;
    }
    co_return all;
}

Task<Result<std::vector<aid::Ticket>>>
OpTicketRepo::findCallTicketsInProjectsOpen(const std::vector<aid::ProjectId>& projects) {
    if (projects.empty())
        co_return std::vector<aid::Ticket>{};

    nlohmann::json projectValues = nlohmann::json::array();
    for (const auto& p : projects)
        projectValues.push_back(p.v);

    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", projectValues}}}});
    filters.push_back({{"type", {{"operator", "="}, {"values", {cfg_.typeCall}}}}});
    // status in {New, InProgress}: OpenProject expresses "or" via operator "=" with multiple
    // values.
    filters.push_back({{"status",
                        {{"operator", "="},
                         {"values",
                          {statusMap_.hrefIdFor(aid::TicketStatus::New).v,
                           statusMap_.hrefIdFor(aid::TicketStatus::InProgress).v}}}}});

    auto open = co_await getAllPaged(multiFilterUrl("/api/v3/work_packages", filters));
    // Warm the handler-drop baseline from this dashboard read (Phase 6/S7): a load
    // is the only path that touches a ticket the daemon has not otherwise fetched
    // since a restart, so without this an admin handler-drop on such a ticket would
    // find a cold ledger and surface no live ticket_remove. recordIfAbsent (not
    // record) so we never clobber a fresher set a write path may have just left;
    // it is in-memory only, so it_query_scope's wire-level budget is unaffected.
    if (open && handlerLedger_ != nullptr) {
        for (const auto& t : *open)
            handlerLedger_->recordIfAbsent(t.id, t.callHandlers);
    }
    co_return open;
}

Task<Result<std::vector<aid::Ticket>>> OpTicketRepo::openCallsInProject(aid::ProjectId project) {
    // [{project=X}, {type=Call}, {status in {New, InProgress}}] — the same
    // open-call predicate as findCallTicketsInProjectsOpen, narrowed to one
    // project. Routed through getAllPaged so a busy project past OpenProject's
    // default 20-row page is not silently truncated. Status "in {New,
    // InProgress}" is operator "=" with both hrefs as values, so Closed
    // tickets are excluded by the query itself.
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"project", {{"operator", "="}, {"values", {project.v}}}}});
    filters.push_back({{"type", {{"operator", "="}, {"values", {cfg_.typeCall}}}}});
    filters.push_back({{"status",
                        {{"operator", "="},
                         {"values",
                          {statusMap_.hrefIdFor(aid::TicketStatus::New).v,
                           statusMap_.hrefIdFor(aid::TicketStatus::InProgress).v}}}}});

    co_return co_await getAllPaged(multiFilterUrl("/api/v3/work_packages", filters));
}

Task<Result<std::vector<aid::Ticket>>>
OpTicketRepo::findOpenTicketsAssignedTo(aid::UserHandle user) {
    auto href = co_await users_.hrefFor(user);
    if (!href)
        co_return unexpected(href.error());

    nlohmann::json filters = nlohmann::json::array();
    // The assignee filter takes the NUMERIC user id, not the "/api/v3/
    // users/4" href — OpenProject 400s on the href form ("Assignee filter
    // has invalid values"). Strip to the trailing id.
    filters.push_back({{"assignee", {{"operator", "="}, {"values", {hrefTail(*href)}}}}});
    filters.push_back(
        {{"status",
          {{"operator", "!"}, {"values", {statusMap_.hrefIdFor(aid::TicketStatus::Closed).v}}}}});

    co_return co_await getAllPaged(multiFilterUrl("/api/v3/work_packages", filters));
}

Task<Result<std::vector<aid::Ticket>>>
OpTicketRepo::findCallTicketsWithHandler(aid::UserHandle viewer) {
    nlohmann::json filters = nlohmann::json::array();
    filters.push_back({{"type", {{"operator", "="}, {"values", {cfg_.typeCall}}}}});
    // status in {New, InProgress}: OpenProject expresses "or" via operator "="
    // with multiple values (same shape as findCallTicketsInProjectsOpen).
    filters.push_back({{"status",
                        {{"operator", "="},
                         {"values",
                          {statusMap_.hrefIdFor(aid::TicketStatus::New).v,
                           statusMap_.hrefIdFor(aid::TicketStatus::InProgress).v}}}}});
    // callHandler CSV contains the viewer's login. "~" is a substring match —
    // the only contains operator OpenProject offers on a text custom field.
    filters.push_back(
        {{customFieldName(fieldMap_.callHandler), {{"operator", "~"}, {"values", {viewer.v}}}}});

    auto all = co_await getAllPaged(multiFilterUrl("/api/v3/work_packages", filters));
    if (!all)
        co_return unexpected(all.error());

    // Substring "~" can over-match (e.g. "alice" inside "malice"/"alice2"), so
    // keep only the tickets where `viewer` is an EXACT entry in the parsed
    // callHandler CSV. This makes the dashboard arm identical to recipientsFor's
    // CSV arm (same rule, inverted) — no ghost rows from a partial-login match.
    std::vector<aid::Ticket> exact;
    exact.reserve(all->size());
    for (auto& t : *all) {
        const bool isHandler =
            std::any_of(t.callHandlers.begin(), t.callHandlers.end(),
                        [&viewer](const aid::UserHandle& h) { return h.v == viewer.v; });
        if (isHandler)
            exact.push_back(std::move(t));
    }

    // Warm the handler-drop baseline from this cross-project dashboard read
    // (Phase 6/S7), the symmetric case to the member arm above: when the viewer is
    // the cross-project handler themselves, loading their dashboard is what
    // establishes the ledger entry an admin's later handler-drop diffs against.
    // recordIfAbsent so a fresher write-path set is never clobbered; in-memory
    // only, so the it_query_scope wire budget is untouched.
    if (handlerLedger_ != nullptr) {
        for (const auto& t : exact)
            handlerLedger_->recordIfAbsent(t.id, t.callHandlers);
    }
    co_return exact;
}

Task<Result<std::vector<aid::UserHandle>>> OpTicketRepo::recipientsFor(const aid::Ticket& t) {
    // Copy out of the reference BEFORE the first co_await. The Task is eager
    // (suspend_never), so the body runs to the suspension point and then
    // resumes later; reading `t` after the co_await would dangle if a future
    // caller passed a temporary (recipientsFor(buildTicket())).
    const aid::ProjectId project = t.projectId;
    const std::vector<aid::UserHandle> handlers = t.callHandlers;

    auto members = co_await users_.projectMembers(project);
    if (!members)
        co_return unexpected(members.error());

    // members ∪ callHandlers, deduped by login, members first for determinism.
    std::vector<aid::UserHandle> out;
    out.reserve(members->size() + handlers.size());
    std::unordered_set<std::string> seen;
    seen.reserve(members->size() + handlers.size());
    for (auto& m : *members) {
        if (seen.insert(m.v).second)
            out.push_back(std::move(m));
    }
    for (const auto& h : handlers) {
        if (seen.insert(h.v).second)
            out.push_back(h);
    }
    co_return out;
}

Task<Result<std::vector<aid::UserHandle>>>
OpTicketRepo::droppedRecipientsOnWebhook(const aid::Ticket& t) {
    // Copy out of the reference BEFORE the first co_await: the Task is eager and
    // `t` may be a temporary that no longer exists when we resume.
    const aid::ProjectId project = t.projectId;
    const aid::TicketId id = t.id;
    const std::vector<aid::UserHandle> current = t.callHandlers;

    if (handlerLedger_ == nullptr)
        co_return std::vector<aid::UserHandle>{};

    // Atomically read the previously-known handler set and install the new one as
    // the baseline for next time. Cold start (untracked / expired) ⇒ nothing can
    // be classified as dropped; the just-installed set seeds the next diff.
    auto prior = handlerLedger_->exchange(id, current);
    if (!prior)
        co_return std::vector<aid::UserHandle>{};

    // dropped = prior \ current — logins that were handlers and no longer are.
    std::vector<aid::UserHandle> dropped;
    for (const auto& was : *prior) {
        const bool stillHandler =
            std::any_of(current.begin(), current.end(),
                        [&was](const aid::UserHandle& h) { return h.v == was.v; });
        if (!stillHandler)
            dropped.push_back(was);
    }
    if (dropped.empty())
        co_return std::vector<aid::UserHandle>{};

    // Only a dropped handler who is ALSO not a project member actually loses
    // visibility — the membership arm would otherwise keep the ticket on their
    // board. Refetch this project's membership (poll-maintained cache, the same
    // source recipientsFor consults) and keep only the non-members. This is the
    // single OpenProject query the webhook path makes, and only when a handler
    // was genuinely removed.
    auto members = co_await users_.projectMembers(project);
    if (!members) {
        // Best-effort self-heal. The membership fetch is the sole error source, but
        // exchange() has ALREADY consumed `prior` and installed `current` as the new
        // baseline — so without intervention the next webhook for this ticket would
        // diff `current` against `current` and the drop would be lost until a
        // dashboard reload. Roll the baseline back to `prior`: customField7 is already
        // at its dropped value, so the next webhook re-delivers `current`, which diffs
        // against the restored `prior` to the SAME dropped set and retries the
        // classification once membership resolves. Under the single domain loop the
        // exchange()→restore pair is effectively atomic for this ticket (webhook
        // decodes for one id are not processed concurrently), and HandlerLedger's
        // mutex keeps each record/exchange individually safe for the
        // concurrent-call contract; in the worst case a fresher write recorded during
        // the membership-fetch suspension is rolled back to `prior`, which the next
        // observation overwrites again and which leaves the REST dashboard correct
        // regardless.
        handlerLedger_->record(id, *prior);
        co_return unexpected(members.error());
    }

    std::vector<aid::UserHandle> surfaced;
    for (const auto& login : dropped) {
        const bool isMember =
            std::any_of(members->begin(), members->end(),
                        [&login](const aid::UserHandle& m) { return m.v == login.v; });
        if (!isMember)
            surfaced.push_back(login);
    }
    co_return surfaced;
}

} // namespace aid::adapters::openproject
