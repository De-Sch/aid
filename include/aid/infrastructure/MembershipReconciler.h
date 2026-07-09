#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/value-types/MembershipDelta.h"

// Poll-driven project-membership reconciler (Phase 1 — the core side of the
// membership poll loop). The ticket system emits NO membership webhook, so a
// long-lived daemon must POLL to notice an admin adding/removing a project
// member (verified live). This component owns the *trigger + gate*; the
// plugin owns the *data* (TicketStore::refreshMembership diff) and the use case
// owns the *diff→push* (usecases/ReconcileMemberships).
//
// Loop affinity: every tick runs on the SAME domain
// loop that owns the plugin's membersCache_ + HttpClient, so every co_await
// inside a tick resumes on that loop — never a cross-loop resumption. The
// recurring *timer*, however, fires on the Drogon app loop and hops onto the
// domain loop via queueInLoop (the same hop kick() uses): a standalone
// trantor::EventLoop's repeating timerfd does not re-arm after its first fire,
// so a domain-loop runEvery would tick exactly once (verified live, S6); the
// app loop's runEvery is reliable. kick() (the 0→1 connect-kick, called from a
// WS listener thread) likewise hops onto the domain loop before touching the
// store.
//
// This is infrastructure (it speaks trantor/Drogon), so per the layering rule it
// must NOT link the use-case layer: the connection gate and the diff→push step
// are injected as std::function callables wired up by Main.

namespace trantor {
class EventLoop;
} // namespace trantor

namespace aid::crosscutting {
class Logger;
} // namespace aid::crosscutting

namespace aid::ports {
class TicketStore;
} // namespace aid::ports

namespace aid::infrastructure {

class MembershipReconciler {
public:
    // "Is ≥1 dashboard WebSocket connected right now?" Cheap, in-memory
    // (WsHubAdapter::anyConnected). Injected so infrastructure does not depend
    // on the ws adapter.
    using ConnectionGate = std::function<bool()>;

    // Apply a batch of deltas → per-recipient live frames. Wraps the
    // ReconcileMemberships use case (infrastructure must not link usecases, so
    // it is injected as a callable). Deltas are passed by value so the wrapped
    // coroutine frame owns them across its internal co_awaits.
    using ApplyDeltas = std::function<aid::plumbing::Task<aid::plumbing::Result<void>>(
        std::vector<aid::MembershipDelta>)>;

    // The poll interval floor (Config clamps to this) so a mis-set tiny value
    // can't hammer the ticket system.
    static constexpr std::chrono::seconds kMinInterval{5};

    MembershipReconciler(trantor::EventLoop& loop, aid::ports::TicketStore& ts,
                         ConnectionGate anyConnected, ApplyDeltas applyDeltas,
                         aid::crosscutting::Logger& logger, std::chrono::seconds interval) noexcept;

    MembershipReconciler(const MembershipReconciler&) = delete;
    MembershipReconciler& operator=(const MembershipReconciler&) = delete;
    MembershipReconciler(MembershipReconciler&&) = delete;
    MembershipReconciler& operator=(MembershipReconciler&&) = delete;
    ~MembershipReconciler();

    // Schedule the recurring poll. Call once after construction, at startup. The
    // timer fires on the Drogon app loop and each tick hops onto the domain loop
    // (see the loop-affinity note above).
    void start();

    // Connect-kick: run ONE immediate reconcile cycle (e.g. on the 0→1
    // WebSocket transition). Safe to call from any thread — it hops onto the
    // domain loop before touching the store.
    void kick();

    // Cancel the timer and wait for any in-flight tick to finish. MUST be
    // called from a NON-loop thread while the domain loop is still alive, before
    // this object is destroyed, so no queued timer/coroutine resumes on a freed
    // `this`. Idempotent. The destructor also calls it as a backstop.
    void stop();

private:
    // Runs on the domain loop: reentrancy-guards, then spawns the detached tick
    // coroutine (a fire-and-forget drogon::AsyncTask defined in the .cpp).
    void launchTick();

    trantor::EventLoop& loop_;
    aid::ports::TicketStore& ts_;
    ConnectionGate anyConnected_;
    ApplyDeltas applyDeltas_;
    aid::crosscutting::Logger& logger_;
    std::chrono::seconds interval_;

    // The loop the recurring timer is registered on (the Drogon app loop, set in
    // start()). Distinct from loop_ (the domain loop the ticks run on). nullptr
    // until start().
    trantor::EventLoop* timerLoop_{nullptr};
    // trantor::TimerId (uint64_t); 0 == InvalidTimerId == "no timer scheduled".
    std::uint64_t timerId_{0};
    // Set by stop(); blocks new ticks from launching.
    std::atomic<bool> stopped_{false};
    // True while a tick coroutine is running; gates out overlapping ticks (a
    // slow ticket system must not pile up cycles) and lets stop() wait for the
    // in-flight tick to finish before `this` is destroyed.
    std::atomic<bool> inFlight_{false};
};

} // namespace aid::infrastructure
