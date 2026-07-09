#include "aid/usecases/HandleIncomingCall.h"

#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

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
                              const aid::IncomingCall& ev, aid::PhoneNumber caller) {
    aid::NewTicket nt;
    nt.projectId = project;
    nt.subject = std::move(subject);
    nt.status = aid::TicketStatus::New;
    nt.callId = ev.callid;
    nt.callerNumber = std::move(caller);
    if (!ev.dialed.empty()) {
        nt.calledNumber = ev.dialed;
    }
    return nt;
}

} // namespace

HandleIncomingCall::HandleIncomingCall(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab,
                                       aid::ports::UiNotifier& ui, aid::crosscutting::Clock& clock,
                                       const aid::crosscutting::Config::TicketRouting& cfg)
    : ts_(ts), ab_(ab), ui_(ui), clock_(clock), cfg_(cfg) {
}

Task<Result<void>> HandleIncomingCall::run(const aid::IncomingCall& ev, bool replay) {
    // Step 0: canonicalize first — synchronous, noexcept. Empty result = incognito.
    const auto canonical = ab_.canonicalize(ev.remote);

    if (canonical.empty()) {
        // WAL-replay idempotency for incognito. The
        // incognito branch below deliberately skips all dedup — distinct live
        // withheld-number calls are distinct events. But the
        // at-least-once WAL window can re-dispatch THIS event after a crash
        // mid-truncate, and the pre-crash run already created its ticket. Only
        // on the replay path, guard on "did I already create a ticket for this
        // exact callid?" — the routable branches need no such guard (they
        // self-dedup by caller number). Live incognito calls (replay == false)
        // fall straight through and always create, exactly as before.
        if (replay) {
            auto existing = co_await ts_.findByExactCallid(ev.callid);
            if (!existing) {
                co_return aid::plumbing::unexpected{existing.error()};
            }
            if (existing->has_value()) {
                // Already created before the crash. Re-emit the live delta
                // (idempotent upsert — covers a crash between create and emit)
                // and skip the create entirely.
                TicketDeltaEmitter emitter{ts_, ui_};
                (void)co_await emitter.emitTicketDelta(std::move(**existing));
                co_return Result<void>{};
            }
        }
        // INCOGNITO BRANCH: no caller id at all (withheld /
        // unparseable). No ab.lookup, NO dedup — every incognito call
        // opens a fresh ticket in the fallback project. We deliberately do
        // NOT roll these into a single rolling ticket: distinct incognito
        // calls are distinct events and must not be merged.
        //
        // callerNumber is the literal "Incognito" (not the raw remote —
        // there is none — and deliberately not "unknown", which is
        // ambiguous with the "routable number absent from the address book" case
        // below). This also satisfies the ticket system's required callerNumber
        // custom field.
        //
        // GCC-12 keeps temporaries materialized in a co_await operand alive
        // in the coroutine frame past their nominal full-expression
        // (double-destroy on frame teardown — repro under ASan). Hoist the
        // NewTicket to a named local first.
        const auto nt = buildNewTicket(cfg_.unknownFallback, cfg_.incognitoSubject, ev,
                                       aid::PhoneNumber{"Incognito"});
        auto created = co_await ts_.create(nt);
        if (!created) {
            co_return aid::plumbing::unexpected{created.error()};
        }
        auto fresh = co_await ts_.fetchById(*created);
        if (fresh.has_value()) {
            TicketDeltaEmitter emitter{ts_, ui_};
            (void)co_await emitter.emitTicketDelta(std::move(*fresh));
        }
        co_return Result<void>{};
    }

    // Routable: step 1 — ab.lookup(canonical). NEVER pass ev.remote.
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
        // KNOWN BRANCH: route into the contact's project(s).
        // Dedup by caller NUMBER (not "newest open call ticket in the
        // project") — reuse an open New/In-Progress ticket only if it
        // already belongs to THIS caller; otherwise create a fresh ticket
        // titled with the contact name. This keeps distinct callers in the
        // same project from merging, matching the unknown-number rule.
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
        // UNKNOWN BRANCH: a routable number with no address-book
        // project mapping. Dedup by caller NUMBER only — repeated calls
        // from the same number roll into its open (New / In-Progress)
        // ticket in the fallback project; otherwise a new one is created.
        //
        // We do NOT look up by subject here: the caller name is usually
        // empty (no contact), and `findOpenInProjectBySubject(project, "")`
        // emits `subject ~ ""`, which the ticket system rejects with HTTP 400.
        // Caller-number is the intended dedup key for unknown numbers.
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

    // Step 4: apply decision. Incoming records no call handler (that happens on
    // accept), so the save path already has the authoritative post-PATCH ticket
    // and can emit from it directly; the create path returns only an id and is
    // re-fetched below.
    std::optional<aid::Ticket> emitSaved;
    std::optional<aid::TicketId> emitCreated;
    if (reuseTicket.has_value()) {
        const aid::TicketId id = reuseTicket->id;
        const aid::CallId callid = ev.callid;
        // Append the new callid as a pure delta on the fresh ticket inside save()
        // (re-applied on every 409), so a concurrent same-ticket writer's callId
        // edit is not clobbered. The reducer is hoisted to a
        // named local to dodge gcc-12's coroutine frame-lifetime bug (see the
        // NewTicket hoist above).
        const aid::ports::TicketReducer reducer = [callid](aid::Ticket t) {
            addCallid(t, callid);
            return t;
        };
        auto saved = co_await ts_.save(id, reducer);
        if (!saved) {
            co_return aid::plumbing::unexpected{saved.error()};
        }
        emitSaved = std::move(*saved);
    } else {
        // See incognito-branch comment: hoist the NewTicket out of the
        // co_await operand to avoid the gcc-12 frame-lifetime bug.
        const auto nt = buildNewTicket(*createInProject, createSubject, ev, canonical);
        auto created = co_await ts_.create(nt);
        if (!created) {
            co_return aid::plumbing::unexpected{created.error()};
        }
        emitCreated = *created;
    }

    // Step 5: push the live delta. clock_ is held for future steps that stamp
    // timestamps; the ring procedure itself doesn't touch it.
    (void)clock_;
    TicketDeltaEmitter emitter{ts_, ui_};
    if (emitSaved.has_value()) {
        (void)co_await emitter.emitTicketDelta(std::move(*emitSaved));
    } else {
        auto fresh = co_await ts_.fetchById(*emitCreated);
        if (fresh.has_value()) {
            (void)co_await emitter.emitTicketDelta(std::move(*fresh));
        }
    }
    co_return Result<void>{};
}

} // namespace aid::usecases
