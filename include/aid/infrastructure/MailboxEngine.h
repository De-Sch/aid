#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace trantor {
class EventLoop;
}

namespace aid::crosscutting {
class Logger;
}

namespace aid::infrastructure {

class Wal;

// Shared per-key event-dispatch engine behind `Mailbox` (keyed by CallId,
// carrying CallEvent) and `WebhookMailbox` (keyed by TicketId, carrying a raw
// std::string body). Same-key events run strictly in order; different keys run
// concurrently on the shared domain loop. Bounded per-key queue, hard cap on
// live keys, crash-safe WAL ack, idle GC, graceful drain, and the
// detached-coroutine frame-lifetime dance — ALL of it lives here exactly once.
//
// The two facades inject only what genuinely differs between them:
//   * the per-event Dispatch (a 5-way std::visit + `replay` flag for calls, a
//     single Handler for webhooks), and
//   * the human-readable Labels (so log/rejection text stays byte-identical to
//     the pre-unification code — live verification greps backend.log).
//
// This is one of the highest-risk components in the system. The bodies were
// lifted verbatim from the two former copies — CallId/TicketId → Key,
// CallEvent/std::string → Payload, and the dispatch step replaced by
// `co_await dispatch_(p)`. Do not refactor the coroutine/lifetime logic.
template <class Key, class Payload> class MailboxEngine {
public:
    struct Pending {
        std::uint64_t walSeq;
        Payload event;
        std::string correlationId;
        // True iff re-enqueued from the WAL on startup (enqueueBypass with
        // replay=true). Only the call flow's incoming/outgoing handlers consume
        // it; the webhook flow always leaves it false. See Mailbox.h for
        // the replay-dedup rationale.
        bool replay = false;
    };

    // The per-event step. Receives the Pending by reference so the call
    // facade can std::visit `event` (and read `replay`) and the webhook facade
    // can std::move the payload out. A copyable closure returning a move-only
    // Task is fine for std::function.
    using Dispatch = std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(Pending&)>;

    // Byte-exact log/rejection text preserved from the two former classes.
    //   prefix       "mailbox"                / "webhook mailbox"
    //   handledLabel "handled event callid"   / "handled ticket"
    //   failLabel    "usecase failed"         / "handler failed"
    struct Labels {
        std::string prefix;
        std::string handledLabel;
        std::string failLabel;
    };

    // domainLoop, wal, and logger must outlive the engine. dispatch and labels
    // are value-captured.
    MailboxEngine(trantor::EventLoop& domainLoop, Wal& wal, aid::crosscutting::Logger& logger,
                  Dispatch dispatch, Labels labels);

    MailboxEngine(const MailboxEngine&) = delete;
    MailboxEngine& operator=(const MailboxEngine&) = delete;
    MailboxEngine(MailboxEngine&&) = delete;
    MailboxEngine& operator=(MailboxEngine&&) = delete;
    ~MailboxEngine();

    // Live path (backpressure applies). Returns Error{InvalidInput, ...} on
    // "{prefix} full" (deque at MAX_QUEUE), "{prefix} cap reached" (new key
    // past MAX_LIVE_MAILBOXES), or "draining" during shutdown. The controller
    // translates each to HTTP 503.
    [[nodiscard]] aid::plumbing::Result<void>
    enqueue(Key key, Payload payload, std::string correlationId, std::uint64_t walSeq, bool replay);

    // Startup replay path. Skips backpressure (the decision was made at
    // original POST time). Callers are responsible for decoding/keying the WAL
    // record before calling this.
    void enqueueBypass(Key key, Payload payload, std::string correlationId, std::uint64_t walSeq,
                       bool replay);

    [[nodiscard]] std::size_t liveCount() const;
    [[nodiscard]] std::size_t failedCount() const noexcept;
    [[nodiscard]] std::size_t trackedMailboxCount() const;

    void gcIdleOlderThan(std::chrono::seconds idle);

    // Graceful-shutdown drain. MUST be called from a non-loop thread — it polls
    // with sleep_for, so calling it from the domain loop deadlocks the very
    // loop the workers need to progress. The concurrency rules whitelist sleep
    // only on the shutdown path; this is it.
    [[nodiscard]] aid::plumbing::Task<void> drain(std::chrono::seconds budget);

    static constexpr std::size_t MAX_QUEUE = 32;
    static constexpr std::size_t MAX_LIVE_MAILBOXES = 10000;

private:
    aid::plumbing::Task<void> workerCoroutine(Key key);
    void spawnWorker(Key key);

    trantor::EventLoop& domainLoop_;
    Wal& wal_;
    aid::crosscutting::Logger& logger_;
    Dispatch dispatch_;
    Labels labels_;

    mutable std::mutex mtx_;
    std::unordered_map<Key, std::deque<Pending>> queues_;
    std::unordered_map<Key, std::chrono::steady_clock::time_point> lastActivity_;
    std::unordered_set<Key> activeWorkers_;
    std::atomic<std::size_t> failedCount_{0};
    std::atomic<bool> draining_{false};

    // Loop-thread-only — holds eager-started worker frames alive across their
    // suspension points. No lock needed: every read/write is posted through
    // domainLoop_.
    std::unordered_map<Key, aid::plumbing::Task<void>> inFlight_;
};

} // namespace aid::infrastructure
