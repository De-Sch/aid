#pragma once

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/Ticket.h"
#include "aid/value-types/WebhookDecode.h"

namespace aid::ports {
class TicketStore;
class UiNotifier;
} // namespace aid::ports

namespace aid::usecases {

// Generic, backend-agnostic live-delta fan-out. Replaces the coarse
// notifyInvalidate("dashboard") broadcast (which forced every connected client
// to refetch the whole dashboard) with a precise per-recipient push:
//
//   recipientsFor(ticket)  →  for each viewer  →  buildEntry(ticket, viewer)
//                                              →  UiNotifier::pushTicketUpsert
//
// A ticket that is no longer on any dashboard (status not New / In Progress —
// e.g. Closed via /ui/close) is removed instead of upserted, so a stale row
// disappears live.
//
// Depends only on the two ports (TicketStore for recipientsFor + buildEntry,
// UiNotifier for the push), so it stays in the use-case layer with no Drogon /
// JSON / adapter coupling. Construct it from the refs a use case already holds.
class TicketDeltaEmitter {
public:
    TicketDeltaEmitter(aid::ports::TicketStore& ts, aid::ports::UiNotifier& ui) noexcept;

    TicketDeltaEmitter(const TicketDeltaEmitter&) = delete;
    TicketDeltaEmitter& operator=(const TicketDeltaEmitter&) = delete;
    TicketDeltaEmitter(TicketDeltaEmitter&&) = delete;
    TicketDeltaEmitter& operator=(TicketDeltaEmitter&&) = delete;
    ~TicketDeltaEmitter() = default;

    // Builds the delta from the POST-SAVE ticket (lockVersion already bumped) —
    // pass save()/create()'s result or a fresh fetchById, never the pre-save
    // ticket. Taken by value so the coroutine frame owns it across the
    // recipientsFor co_await (no dangling on a temporary argument). Errors come
    // only from recipientsFor; callers treat them as non-fatal — the persisted
    // change already succeeded and the dashboard is eventually consistent.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    emitTicketDelta(aid::Ticket ticket);

    // Webhook fan-out (Phase 6 / S8). Given the WebhookDecode of a genuine
    // (non-echo) ticket-system edit, first run the usual per-recipient upsert
    // delta for the ticket's CURRENT recipients (= emitTicketDelta), then — AFTER
    // that fan-out — push a ticket_remove to each login an admin dropped from the
    // callHandler set who is no longer a project member (decode.droppedRecipients).
    // Those dropped logins are disjoint from the current recipients and were
    // computed plugin-side by decodeWebhook, so the removes fire independently of
    // recipientsFor — even when the upsert fan-out fails. Returns the upsert
    // delta's Result (the removes are unconditional, best-effort fire-and-forget).
    // Taken by value so the coroutine frame owns the ticket + dropped logins
    // across emitTicketDelta's internal co_await.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>>
    emitWebhookDelta(aid::WebhookDecode decode);

private:
    aid::ports::TicketStore& ts_;
    aid::ports::UiNotifier& ui_;
};

} // namespace aid::usecases
