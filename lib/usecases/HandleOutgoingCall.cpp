#include "aid/usecases/HandleOutgoingCall.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "aid/crosscutting/Clock.h"
#include "aid/domain/CallTracker.h"
#include "aid/domain/TicketRouter.h"
#include "aid/plumbing/Error.h"
#include "aid/ports/AddressBook.h"
#include "aid/ports/TicketStore.h"
#include "aid/ports/UiNotifier.h"
#include "aid/usecases/TicketDeltaEmitter.h"
#include "aid/value-types/Contact.h"
#include "aid/value-types/Ids.h"
#include "aid/value-types/Ticket.h"

namespace aid::usecases {

namespace {

using aid::plumbing::Result;
using aid::plumbing::Task;

std::string contactName(const aid::Contact& c) {
    return c.name.empty() ? c.companyName : c.name;
}

void addCallid(aid::Ticket& t, const aid::CallId& id) {
    const auto encoded = aid::domain::CallTracker::encode(t.callIds);
    const auto updated = aid::domain::CallTracker::withAdded(encoded, id);
    t.callIds = aid::domain::CallTracker::decode(updated);
}

aid::NewTicket buildNewTicket(const aid::ProjectId& project, std::string subject,
                              const aid::OutgoingCall& ev, aid::PhoneNumber caller,
                              const aid::UserHandle& assignee) {
    aid::NewTicket nt;
    nt.projectId = project;
    nt.subject = std::move(subject);
    // An outgoing call is placed BY a known agent, so the ticket is born
    // actively-handled, not pending pickup: create it directly InProgress
    // (mirrors HandleAcceptedCall's New→InProgress flip). Unlike incoming —
    // which rings unassigned/New until someone accepts — outgoing has no
    // separate Accept event in the wire protocol (calls.py on_accept is
    // from-trunk/incoming only), so this is the sole place it can be marked
    // in-progress.
    nt.status = aid::TicketStatus::InProgress;
    nt.callId = ev.callid;
    nt.callerNumber = std::move(caller);
    // Outgoing has no `dialed` field — calledNumber stays nullopt.
    nt.assignee = assignee;
    return nt;
}

} // namespace

HandleOutgoingCall::HandleOutgoingCall(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab,
                                       aid::ports::UiNotifier& ui, aid::crosscutting::Clock& clock,
                                       const aid::crosscutting::Config::TicketRouting& cfg)
    : ts_(ts), ab_(ab), ui_(ui), clock_(clock), cfg_(cfg) {
}

Task<Result<void>> HandleOutgoingCall::run(const aid::OutgoingCall& ev, bool replay) {
    // Step 1: resolve user. Missing user is non-fatal — drop the event.
    auto resolved = co_await ts_.resolveUser(ev.user.v);
    if (!resolved) {
        co_return aid::plumbing::unexpected{resolved.error()};
    }
    if (!resolved->has_value()) {
        co_return Result<void>{};
    }
    const aid::UserHandle user = **resolved;

    // Step 2: canonicalize first — synchronous, noexcept. Empty = incognito.
    const auto canonical = ab_.canonicalize(ev.remote);

    if (canonical.empty()) {
        // WAL-replay idempotency for incognito — mirror
        // of HandleIncomingCall. A live incognito call always creates,
        // but a replayed one must not double-create the ticket the pre-crash
        // run already made. Only on the replay path, dedup by exact callid.
        if (replay) {
            auto existing = co_await ts_.findByExactCallid(ev.callid);
            if (!existing) {
                co_return aid::plumbing::unexpected{existing.error()};
            }
            if (existing->has_value()) {
                const aid::TicketId id = (**existing).id;
                // Heal partial completion: re-record the operator (addCallHandler
                // is idempotent — appends only if absent) in case the crash
                // landed between create and addCallHandler, then re-emit the
                // delta from the authoritative ticket. Skip the create.
                auto recorded = co_await ts_.addCallHandler(id, user);
                if (!recorded) {
                    co_return aid::plumbing::unexpected{recorded.error()};
                }
                auto fresh = co_await ts_.fetchById(id);
                if (fresh.has_value()) {
                    TicketDeltaEmitter emitter{ts_, ui_};
                    (void)co_await emitter.emitTicketDelta(std::move(*fresh));
                }
                co_return Result<void>{};
            }
        }

        // INCOGNITO BRANCH: no ab.lookup, NO dedup — always create
        // a fresh ticket. callerNumber = literal "Incognito" (see
        // HandleIncomingCall for the rationale), assignee = user.
        //
        // GCC-12 frame-lifetime workaround: hoist NewTicket to a named
        // local so it doesn't outlive its full-expression in the coroutine
        // frame (see HandleIncomingCall.cpp).
        const auto nt = buildNewTicket(cfg_.unknownFallback, cfg_.incognitoSubject, ev,
                                       aid::PhoneNumber{"Incognito"}, user);
        auto created = co_await ts_.create(nt);
        if (!created) {
            co_return aid::plumbing::unexpected{created.error()};
        }
        // Record the calling operator in the callHandler CSV (visibility
        // mechanism, propagate on failure so the WAL can replay).
        auto recorded = co_await ts_.addCallHandler(*created, user);
        if (!recorded) {
            co_return aid::plumbing::unexpected{recorded.error()};
        }
        (void)clock_; // held for later usecases; ring procedure doesn't use it.
        auto fresh = co_await ts_.fetchById(*created);
        if (fresh.has_value()) {
            TicketDeltaEmitter emitter{ts_, ui_};
            (void)co_await emitter.emitTicketDelta(std::move(*fresh));
        }
        co_return Result<void>{};
    }

    // Routable: step 3 — ab.lookup(canonical). NEVER pass ev.remote.
    auto contactRes = co_await ab_.lookup(canonical);
    if (!contactRes) {
        co_return aid::plumbing::unexpected{contactRes.error()};
    }
    const std::optional<aid::Contact>& contactOpt = *contactRes;
    const bool isKnown = contactOpt.has_value() && !contactOpt->projectIds.empty();

    std::optional<aid::Ticket> reuseTicket;
    std::optional<aid::ProjectId> createInProject;
    std::string createSubject;

    if (isKnown) {
        // KNOWN BRANCH: route into the contact's project(s), dedup by caller
        // NUMBER (reuse an open ticket only if it already belongs to this
        // caller; else create). Symmetric with HandleIncomingCall.
        const aid::Contact& contact = *contactOpt;
        createSubject = contactName(contact);

        std::vector<aid::domain::TicketRouter::RoutingCandidate> candidates;
        candidates.reserve(contact.projectIds.size());
        for (const auto& pid : contact.projectIds) {
            auto open = co_await ts_.findOpenInProjectByCallerNumber(pid, canonical);
            if (!open) {
                co_return aid::plumbing::unexpected{open.error()};
            }
            candidates.push_back(
                aid::domain::TicketRouter::RoutingCandidate{pid, std::move(*open)});
        }

        const auto decision = aid::domain::TicketRouter::decideKnown(
            aid::domain::TicketRouter::KnownInput{std::span{candidates}});
        if (const auto* re = std::get_if<aid::domain::TicketRouter::ReuseExisting>(&decision)) {
            for (auto& c : candidates) {
                if (c.latestOpenCallTicket.has_value() &&
                    c.latestOpenCallTicket->id == re->ticket) {
                    reuseTicket = std::move(c.latestOpenCallTicket);
                    break;
                }
            }
        } else {
            const auto& cp = std::get<aid::domain::TicketRouter::CreateInProject>(decision);
            createInProject = cp.project;
        }
    } else {
        // UNKNOWN BRANCH: dedup by caller NUMBER only. No
        // by-subject lookup — an empty caller name yields `subject ~ ""`,
        // which the ticket system rejects with HTTP 400 (see HandleIncomingCall).
        std::string name;
        if (contactOpt.has_value()) {
            name = contactName(*contactOpt);
        }
        createSubject = name.empty() ? canonical.v : name;

        auto byNumber =
            co_await ts_.findOpenInProjectByCallerNumber(cfg_.unknownFallback, canonical);
        if (!byNumber) {
            co_return aid::plumbing::unexpected{byNumber.error()};
        }

        const auto decision =
            aid::domain::TicketRouter::decideUnknown(aid::domain::TicketRouter::UnknownInput{
                std::optional<aid::Ticket>{}, *byNumber, cfg_.unknownFallback});
        if (const auto* re = std::get_if<aid::domain::TicketRouter::ReuseExisting>(&decision)) {
            if (byNumber->has_value() && (**byNumber).id == re->ticket) {
                reuseTicket = std::move(*byNumber);
            }
        } else {
            const auto& cp = std::get<aid::domain::TicketRouter::CreateInProject>(decision);
            createInProject = cp.project;
        }
    }

    // Step 4: apply decision. The calling operator is recorded in the
    // callHandler CSV either way; the assignee is set ONLY when empty (the CSV,
    // not the single assignee, is the visibility mechanism — see addCallHandler).
    aid::TicketId emitId;
    if (reuseTicket.has_value()) {
        const aid::TicketId id = reuseTicket->id;
        const aid::CallId callid = ev.callid;
        // Append the new callid and set the assignee (only when empty) as a pure
        // delta on the fresh ticket inside save() (re-applied on every 409), so a
        // concurrent same-ticket writer's callId edit is not clobbered. The
        // reducer is hoisted to a named local to dodge
        // gcc-12's coroutine frame-lifetime bug (see the NewTicket hoist above).
        const aid::ports::TicketReducer reducer = [callid, user](aid::Ticket t) {
            addCallid(t, callid);
            if (!t.assignee.has_value()) {
                t.assignee = user;
            }
            // An outgoing call actively engages the reused ticket → InProgress,
            // same as the create path and HandleAcceptedCall. Never reopen a
            // Closed ticket (invariant); the reuse candidates already
            // exclude Closed, so this guard is belt-and-suspenders.
            if (t.status != aid::TicketStatus::Closed) {
                t.status = aid::TicketStatus::InProgress;
            }
            return t;
        };
        auto saved = co_await ts_.save(id, reducer);
        if (!saved) {
            co_return aid::plumbing::unexpected{saved.error()};
        }
        auto recorded = co_await ts_.addCallHandler(id, user);
        if (!recorded) {
            co_return aid::plumbing::unexpected{recorded.error()};
        }
        emitId = id;
    } else {
        const auto nt = buildNewTicket(*createInProject, createSubject, ev, canonical, user);
        auto created = co_await ts_.create(nt);
        if (!created) {
            co_return aid::plumbing::unexpected{created.error()};
        }
        auto recorded = co_await ts_.addCallHandler(*created, user);
        if (!recorded) {
            co_return aid::plumbing::unexpected{recorded.error()};
        }
        emitId = *created;
    }

    // Push the live delta from the authoritative post-PATCH ticket (re-fetched
    // after the save/create AND the callHandler merge). Non-fatal on failure.
    (void)clock_;
    auto fresh = co_await ts_.fetchById(emitId);
    if (fresh.has_value()) {
        TicketDeltaEmitter emitter{ts_, ui_};
        (void)co_await emitter.emitTicketDelta(std::move(*fresh));
    }
    co_return Result<void>{};
}

} // namespace aid::usecases
