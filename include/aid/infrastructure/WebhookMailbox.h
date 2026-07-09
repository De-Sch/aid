#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

#include "aid/infrastructure/MailboxEngine.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/plumbing/WalRecord.h"
#include "aid/value-types/Ids.h"

namespace trantor {
class EventLoop;
}

namespace aid::crosscutting {
class Logger;
}

namespace aid::infrastructure {

class Wal;

// Suppress implicit instantiation everywhere — the definition lives once in
// MailboxEngine.cpp.
extern template class MailboxEngine<aid::TicketId, std::string>;

// Per-ticket webhook dispatcher (Phase 6). A thin typed facade over
// MailboxEngine<TicketId, std::string>: keyed by TicketId and carrying the raw
// webhook body, it owns a single Handler and a KeyExtractor and injects the
// webhook-specific dispatch (hand the payload to the Handler). Webhooks for the
// SAME ticket run strictly in order; different tickets run concurrently. The
// shared concurrency/lifetime machinery lives in the engine — see
// MailboxEngine.h.
//
// In production the Handler decodes the webhook via TicketStore::decodeWebhook
// and, on a non-echo result, emits the same per-recipient ticket delta the call
// flow emits. On Handler success the originating WAL record is acked.
class WebhookMailbox {
    using Engine = MailboxEngine<aid::TicketId, std::string>;

public:
    // Process one webhook body. The correlationId rides along for logging. A
    // returned Error (or a thrown exception) leaves the WAL record in place for
    // replay, exactly like the call Mailbox.
    using Handler = std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(
        std::string payload, std::string correlationId)>;

    // Recover the ticket-id key from a stored webhook body on replay. Empty
    // optional => the record cannot be keyed and is dropped (with a warn).
    using KeyExtractor = std::function<std::optional<aid::TicketId>(std::string_view)>;

    // domainLoop, wal, and logger must outlive the WebhookMailbox. extractor may
    // be empty for callers that never replay; an empty extractor makes
    // enqueueReplay log and drop.
    WebhookMailbox(trantor::EventLoop& domainLoop, Wal& wal, aid::crosscutting::Logger& logger,
                   Handler handler, KeyExtractor extractor);

    WebhookMailbox(const WebhookMailbox&) = delete;
    WebhookMailbox& operator=(const WebhookMailbox&) = delete;
    WebhookMailbox(WebhookMailbox&&) = delete;
    WebhookMailbox& operator=(WebhookMailbox&&) = delete;

    // Backpressure mirror of Mailbox::enqueue: Error{InvalidInput, "webhook
    // mailbox full"} at MAX_QUEUE, "webhook mailbox cap reached" past
    // MAX_LIVE_MAILBOXES, "draining" during graceful shutdown. The controller
    // translates each to HTTP 503.
    [[nodiscard]] aid::plumbing::Result<void> enqueue(aid::TicketId ticketId, std::string payload,
                                                      std::string correlationId,
                                                      std::uint64_t walSeq);

    // Startup-only path. Skips backpressure. Drops the record (with a warn) if
    // the extractor returns nullopt or no extractor was provided.
    void enqueueReplay(const aid::plumbing::WalRecord& rec);

    [[nodiscard]] std::size_t liveCount() const;
    [[nodiscard]] std::size_t failedCount() const noexcept;
    [[nodiscard]] std::size_t trackedMailboxCount() const;

    void gcIdleOlderThan(std::chrono::seconds idle);

    // Graceful-shutdown drain (mirror of Mailbox::drain). MUST be called from a
    // non-loop thread — see MailboxEngine.h.
    [[nodiscard]] aid::plumbing::Task<void> drain(std::chrono::seconds budget);

    static constexpr std::size_t MAX_QUEUE = Engine::MAX_QUEUE;
    static constexpr std::size_t MAX_LIVE_MAILBOXES = Engine::MAX_LIVE_MAILBOXES;

private:
    // The webhook-specific per-event step: hand the raw body to the Handler.
    aid::plumbing::Task<aid::plumbing::Result<void>> dispatch(Engine::Pending& p);

    aid::crosscutting::Logger& logger_; // for enqueueReplay warn text
    Handler handler_;
    KeyExtractor extractor_;
    // Declared LAST: ~Engine runs its shutdown-barrier flush before handler_
    // and extractor_ (which the dispatch closure captures) are destroyed.
    Engine engine_;
};

} // namespace aid::infrastructure
