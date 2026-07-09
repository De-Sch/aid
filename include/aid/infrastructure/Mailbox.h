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
#include "aid/value-types/CallEvent.h"
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
extern template class MailboxEngine<aid::CallId, aid::CallEvent>;

// Per-callid event dispatcher. Same-callid events run strictly in order;
// different callids run concurrently on the shared domain loop. A thin typed
// facade over MailboxEngine<CallId, CallEvent>: it owns the five per-event
// handlers and the replay decoder and injects the call-specific dispatch
// (a 5-way std::visit + `replay` flag). The concurrency/lifetime machinery
// lives in the engine — see MailboxEngine.h.
//
// Scaffolding note: the spec lists `HandleIncomingCall&` … `HandleHangup&`
// as members and `CallController::decode` as the replay parser. Main wires
// concrete use cases in as std::function callbacks (one lambda per slot) plus
// a decoder callback at construction.
class Mailbox {
    using Engine = MailboxEngine<aid::CallId, aid::CallEvent>;

public:
    // The incoming/outgoing handlers receive the per-event `replay` flag; the
    // others route by callid (self-deduping) and never create blindly, so they
    // need no replay context.
    using IncomingHandler = std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(
        const aid::IncomingCall&, bool replay)>;
    using OutgoingHandler = std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(
        const aid::OutgoingCall&, bool replay)>;
    using AcceptedHandler =
        std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(const aid::AcceptedCall&)>;
    using TransferHandler =
        std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(const aid::TransferCall&)>;
    using HangupHandler =
        std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(const aid::HangupCall&)>;
    using ReplayDecoder = std::function<std::optional<aid::CallEvent>(std::string_view)>;

    struct Handlers {
        IncomingHandler incoming;
        OutgoingHandler outgoing;
        AcceptedHandler accepted;
        TransferHandler transfer;
        HangupHandler hangup;
    };

    // domainLoop, wal, and logger must outlive the Mailbox. Decoder may be
    // empty for production callers that never call enqueueReplay; an empty
    // decoder causes enqueueReplay to log and drop.
    Mailbox(trantor::EventLoop& domainLoop, Wal& wal, aid::crosscutting::Logger& logger,
            Handlers handlers, ReplayDecoder decoder);

    Mailbox(const Mailbox&) = delete;
    Mailbox& operator=(const Mailbox&) = delete;
    Mailbox(Mailbox&&) = delete;
    Mailbox& operator=(Mailbox&&) = delete;

    // Backpressure: returns Error{InvalidInput, "mailbox full"} when the
    // per-callid deque is at MAX_QUEUE, Error{InvalidInput, "mailbox cap
    // reached"} when adding a new callid would exceed MAX_LIVE_MAILBOXES,
    // and Error{InvalidInput, "draining"} during graceful shutdown. The
    // controller is responsible for translating each to HTTP 503.
    [[nodiscard]] aid::plumbing::Result<void> enqueue(aid::CallId callid, aid::CallEvent event,
                                                      std::string correlationId,
                                                      std::uint64_t walSeq);

    // Startup-only path (Main calls this after Wal::readAll). Skips
    // backpressure: the backpressure decision was made at original POST
    // time. Drops the record (with a warn log) if the decoder returns
    // nullopt or if no decoder was provided.
    void enqueueReplay(const aid::plumbing::WalRecord& rec);

    [[nodiscard]] std::size_t liveCount() const;
    [[nodiscard]] std::size_t failedCount() const noexcept;
    [[nodiscard]] std::size_t trackedMailboxCount() const;

    void gcIdleOlderThan(std::chrono::seconds idle);

    // Graceful shutdown helper. MUST be called from a non-loop thread
    // (typically the SIGTERM handler on the main thread) — see MailboxEngine.h.
    [[nodiscard]] aid::plumbing::Task<void> drain(std::chrono::seconds budget);

    static constexpr std::size_t MAX_QUEUE = Engine::MAX_QUEUE;
    static constexpr std::size_t MAX_LIVE_MAILBOXES = Engine::MAX_LIVE_MAILBOXES;

private:
    // The call-specific per-event step: std::visit over the CallEvent variant
    // to the matching handler, passing `replay` to incoming/outgoing.
    aid::plumbing::Task<aid::plumbing::Result<void>> dispatch(Engine::Pending& p);

    aid::crosscutting::Logger& logger_; // for enqueueReplay warn text
    Handlers handlers_;
    ReplayDecoder decoder_;
    // Declared LAST: ~Engine runs its shutdown-barrier flush before handlers_
    // and decoder_ (which the dispatch closure captures) are destroyed.
    Engine engine_;
};

} // namespace aid::infrastructure
