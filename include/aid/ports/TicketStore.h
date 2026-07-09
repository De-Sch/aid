#pragma once

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/MembershipDelta.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

// Abstract port for ticket persistence. Implementations live
// in plugin .so's and link only aid_ports + stdlib — no Drogon, no nlohmann::json.

namespace aid::ports {

// A pure delta applied to a freshly fetched ticket to produce the ticket to
// persist. See TicketStore::save below for the full contract — the reducer
// MUST be pure and total, and the adapter may invoke it 1..N times (once per
// optimistic-lock attempt), each time against the latest server state.
using TicketReducer = std::function<Ticket(Ticket)>;

class TicketStore {
public:
    virtual ~TicketStore() = default;

    TicketStore() = default;
    TicketStore(const TicketStore&) = delete;
    TicketStore& operator=(const TicketStore&) = delete;
    TicketStore(TicketStore&&) = delete;
    TicketStore& operator=(TicketStore&&) = delete;

    // Shutdown hook. Best-effort: abort every upstream
    // request this port currently has in flight so any worker coroutine
    // suspended inside one of its methods resumes at once with a terminal error
    // instead of waiting out the read timeout. The daemon calls this DURING the
    // graceful drain — while the port is still fully alive — so the resumed
    // chains unwind through live port internals and reach final_suspend before
    // the port is destroyed; the daemon then waits for the mailboxes to go
    // quiescent and only then releases the plugin. Must be safe to call from a
    // non-loop thread and idempotent. Default: no-op (a port with no
    // cancellable in-flight state does nothing).
    virtual void cancelPendingRequests() noexcept {}

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<Ticket>> fetchById(TicketId id) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Ticket>>>
    findByExactCallid(CallId id) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Ticket>>>
    findByCallidContains(CallId id) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Ticket>>>
    findLatestOpenCallInProject(ProjectId project) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Ticket>>>
    findOpenInProjectBySubject(ProjectId project, std::string_view subject) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<Ticket>>>
    findOpenInProjectByCallerNumber(ProjectId project, PhoneNumber caller) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<UserHandle>>>
    resolveUser(std::string_view login) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::vector<ProjectId>>>
    listProjectsForUser(UserHandle user) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::vector<DashboardEntry>>>
    listDashboard(UserHandle viewer) = 0;

    // Project a single ticket to the per-viewer DashboardEntry exactly as
    // listDashboard would (same href, activeCallForViewer, otherActiveUsers,
    // statusId, lockVersion). Synchronous and pure — no I/O — because the
    // projection is a config-only transform; the live-delta emitter
    // (usecases/TicketDeltaEmitter) reuses it so a pushed ticket_upsert is
    // byte-identical to a row the REST dashboard would have returned.
    [[nodiscard]] virtual DashboardEntry buildEntry(const Ticket& ticket, UserHandle viewer) = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<TicketId>>
    create(const NewTicket& ticket) = 0;

    // Persist a mutation to the ticket `id`, expressed as a pure delta rather
    // than an absolute ticket value. The adapter fetches the ticket's current
    // server state, applies `reduce` to it, and PATCHes the result; on an
    // optimistic-lock conflict (ticket-system HTTP 409) it RE-FETCHES the now-fresh
    // state and applies `reduce` AGAIN before retrying. Returns the persisted
    // ticket (post-write lockVersion).
    //
    // The reducer MUST be PURE and TOTAL:
    //   - Pure: no I/O, no captured mutable state, no dependence on call count.
    //     It may be invoked 1..N times for a single save (once per attempt).
    //   - Total: defined for ANY ticket state it is handed — it sees only the
    //     freshly fetched ticket; lockVersion, HTTP, and the retry machinery
    //     never appear in its world.
    //   - It must express its change as a DELTA on the ticket it is given
    //     (read `fresh.callLength` / `fresh.callIds` / `fresh.description` and
    //     edit them), NEVER as a wholesale replacement with a value captured
    //     from an earlier read. Doing the latter reintroduces the lost-update
    //     race this method exists to prevent: a concurrent
    //     same-ticket writer's edit on a delta field would be clobbered.
    //
    // Absolute-target fields the caller fully owns (status, assignee, subject,
    // timestamps) may be set to a definite value in the reducer — those are
    // last-writer-wins by intent. Only the read-modify-write delta fields
    // require the read-from-`fresh` discipline above.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<Ticket>> save(TicketId id,
                                                                        TicketReducer reduce) = 0;

    // Record `login` as a handler of the ticket, appending only if absent
    // (dedup). A backend that allows only one assignee (e.g. the ticket system) cannot keep
    // every accept/transfer/outgoing operator visible through the assignee alone;
    // this is the visibility mechanism that survives independent of project
    // membership. Implementations must make the merge concurrency-safe so two
    // mailboxes recording different handlers on one ticket do not clobber each
    // other. A no-op-friendly null backend may simply return success.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<void>>
    addCallHandler(TicketId id, UserHandle login) = 0;

    // The set of users who should see `ticket`: the members of its project
    // UNION the logins recorded as its call handlers, deduped. This is the
    // exact inverse of listDashboard's visibility rule — a ticket appears on a
    // viewer's dashboard iff that viewer is among recipientsFor(ticket) — and a
    // later phase uses it to decide which live WebSocket sessions to notify. A
    // backend without project membership may return just the handlers.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::vector<UserHandle>>>
    recipientsFor(const Ticket& ticket) = 0;

    // The open (New + InProgress) call tickets in a single project. The
    // membership reconciler runs this ONLY for a project whose member set just
    // changed, to recompute who should see each of its open call tickets — so
    // it is deliberately membership-agnostic itself (project + open-status +
    // type=Call only). Implementations must page the full collection, never
    // truncating at the backend's default page size. A backend without a
    // project concept may return an empty vector.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::vector<Ticket>>>
    openCallsInProject(ProjectId project) = 0;

    // Re-fetch the membership of every project the store currently tracks, diff
    // each against the cached set, update the cache, and return one
    // MembershipDelta per project whose member set changed (added / removed
    // logins). Projects whose set is unchanged produce no entry. This is how a
    // long-lived daemon notices an admin adding/removing a project member —
    // The ticket system emits no membership webhook, so the change must be polled.
    //
    // SAFETY CONTRACT (mandatory): a failed / non-2xx / permission-empty fetch
    // MUST be treated as "no change" — keep the cache, emit no delta. An error
    // must never be read as "all members removed", which would mass-evict
    // tickets from every affected dashboard. A backend without project
    // membership may return an empty vector.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::vector<MembershipDelta>>>
    refreshMembership() = 0;

    [[nodiscard]] virtual plumbing::Task<plumbing::Result<void>> close(TicketId id) = 0;

    // Decode an inbound ticket-system webhook body into the WebhookDecode it
    // describes, suppressing the daemon's own echoes (Phase 6 — reflect edits made
    // directly in the ticket-system UI). The adapter parses the payload into a
    // Ticket@Vn, waits a short grace period, then:
    //   - returns std::nullopt when Vn is recorded in the adapter's
    //     produced-version ledger for that ticket id (a self-induced echo of a
    //     create()/save() this daemon just performed — must not re-emit), or
    //   - returns a WebhookDecode otherwise (a genuine external edit): its
    //     `ticket` is the parsed Ticket and `droppedRecipients` are the logins an
    //     admin removed from the callHandler CSV who are no longer project
    //     members and so must be sent a ticket_remove. droppedRecipients is empty
    //     for an edit that drops no handler and for the cold-start case (no prior
    //     handler set known for the ticket — e.g. the first webhook after a
    //     restart; it self-heals on the recipient's next dashboard reload).
    // The echo match is on the EXACT (ticketId, lockVersion) pair — never a
    // blanket time window — so a human edit landing at V+2 within the grace
    // window still passes through. A backend with no webhook
    // contract may return Error{InvalidInput}. `payload` is taken by value so the
    // coroutine frame owns the bytes across its internal delay.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<std::optional<WebhookDecode>>>
    decodeWebhook(std::string payload) = 0;

    // Cheap reachability probe for HealthService::bootstrapPing.
    // the ticket-system adapter's implementation hits a cheap authenticated GET; any 2xx → ok,
    // anything else → Error{UpstreamUnavailable, ...}. Implementations must
    // not throw across the plugin ABI.
    [[nodiscard]] virtual plumbing::Task<plumbing::Result<void>> ping() = 0;
};

} // namespace aid::ports
