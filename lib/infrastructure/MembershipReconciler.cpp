#include "aid/infrastructure/MembershipReconciler.h"

#include <drogon/HttpAppFramework.h>
#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <exception>
#include <future>
#include <string>
#include <thread>
#include <utility>

#include "aid/crosscutting/Logger.h"
#include "aid/ports/TicketStore.h"

namespace aid::infrastructure {

using aid::plumbing::Result;
using aid::plumbing::Task;

MembershipReconciler::MembershipReconciler(trantor::EventLoop& loop, aid::ports::TicketStore& ts,
                                           ConnectionGate anyConnected, ApplyDeltas applyDeltas,
                                           aid::crosscutting::Logger& logger,
                                           std::chrono::seconds interval) noexcept
    : loop_(loop), ts_(ts), anyConnected_(std::move(anyConnected)),
      applyDeltas_(std::move(applyDeltas)), logger_(logger), interval_(interval) {
}

MembershipReconciler::~MembershipReconciler() {
    // Backstop only — the documented teardown path is an explicit stop() while
    // the domain loop is still alive. If stop() already ran, this is a guarded
    // no-op; if it didn't, this still requires the domain loop to be alive (same
    // contract as stop()).
    stop();
}

void MembershipReconciler::start() {
    // The recurring cadence runs on the Drogon app/main loop — NOT the domain
    // loop. A standalone trantor::EventLoop's *repeating* timerfd does not re-arm
    // after its first fire (verified live: runEvery on the hand-rolled domain
    // loop is effectively one-shot), whereas the app loop's runEvery is
    // rock-solid — it already drives the session-prune, mailbox-GC and SIGTERM
    // drain timers. Each tick hops onto the domain loop via queueInLoop so the
    // reconcile keeps the plugin's membersCache_/HttpClient loop affinity —
    // the very same hop kick() uses. The hop also
    // means launchTick never runs in the timer-callback context, so the
    // domain-loop timerfd re-arm bug is sidestepped entirely.
    timerLoop_ = drogon::app().getLoop();
    timerId_ = timerLoop_->runEvery(static_cast<double>(interval_.count()),
                                    [this]() { loop_.queueInLoop([this]() { launchTick(); }); });
}

void MembershipReconciler::kick() {
    if (stopped_.load(std::memory_order_acquire)) {
        return;
    }
    // Hop onto the domain loop: kick() is called from a WS listener thread, but
    // the store's membersCache_/HttpClient have domain-loop affinity.
    loop_.queueInLoop([this]() { launchTick(); });
}

void MembershipReconciler::stop() {
    if (stopped_.exchange(true))
        return;

    // Cancel the recurring timer on the app loop so no further tick is queued
    // onto the domain loop. At process teardown the app loop has already
    // returned from run() before stop() is reached, so guard on isRunning():
    // queueInLoop + wait on a stopped loop would block forever, and a stopped
    // loop fires no more timers anyway.
    if (timerId_ != 0 && timerLoop_ != nullptr && timerLoop_->isRunning()) {
        if (timerLoop_->isInLoopThread()) {
            timerLoop_->invalidateTimer(timerId_);
        } else {
            std::promise<void> done;
            auto fut = done.get_future();
            timerLoop_->queueInLoop([this, &done]() {
                timerLoop_->invalidateTimer(timerId_);
                done.set_value();
            });
            fut.wait();
        }
    }

    // Flush the domain loop so any launchTick hop already queued (by the timer or
    // a kick that won the race before stopped_ was set) runs — it no-ops now that
    // stopped_ is set — before `this` is destroyed. Closes the window where a
    // queued hop could otherwise resume on a freed `this`. The domain loop is
    // still alive here (it is torn down later than the reconciler).
    if (loop_.isRunning() && !loop_.isInLoopThread()) {
        std::promise<void> flushed;
        auto fut = flushed.get_future();
        loop_.queueInLoop([&flushed]() { flushed.set_value(); });
        fut.wait();
    }

    // Wait for any in-flight tick coroutine (suspended inside refreshMembership
    // on the domain loop) to finish so it can't resume on a freed `this`. The
    // tick runs on the domain loop; we poll its completion flag (bounded by the
    // upstream HttpClient timeout).
    while (inFlight_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds{5});
    }
}

void MembershipReconciler::launchTick() {
    // Reentrancy guard: skip if a previous cycle is still running so a slow
    // ticket system can't pile up overlapping ticks. CAS so only the winner owns
    // the in-flight flag and is responsible for clearing it.
    bool expected = false;
    if (!inFlight_.compare_exchange_strong(expected, true)) {
        return;
    }
    if (stopped_.load(std::memory_order_acquire)) {
        inFlight_.store(false, std::memory_order_release);
        return;
    }

    // Detached fire-and-forget coroutine on this (the domain) loop, mirroring
    // Main's bootstrap-ping pattern. drogon::AsyncTask self-destructs on
    // completion (suspend_never on both suspends), so there is no frame to keep
    // alive. The top-level try/catch is mandatory: Drogon swallows
    // exceptions thrown on a detached coroutine.
    [](MembershipReconciler* self) -> drogon::AsyncTask {
        try {
            // Gate: only poll while ≥1 dashboard is watching. An idle daemon
            // makes zero membership round-trips.
            if (self->anyConnected_ && self->anyConnected_()) {
                auto deltas = co_await self->ts_.refreshMembership();
                if (!deltas) {
                    // Safety guard lives in the plugin (an errored fetch never
                    // diffs to "all removed"); here we just surface it — this is
                    // the only place the failure reaches backend.log.
                    self->logger_.warn("MembershipReconciler: refreshMembership failed: " +
                                       deltas.error().message);
                } else if (!deltas->empty()) {
                    // Heavy openCallsInProject query is gated on a real change
                    // (it lives inside applyDeltas / ReconcileMemberships).
                    if (auto r = co_await self->applyDeltas_(std::move(*deltas)); !r) {
                        self->logger_.warn("MembershipReconciler: reconcile failed: " +
                                           r.error().message);
                    }
                }
            }
        } catch (const std::exception& e) {
            self->logger_.error(std::string{"MembershipReconciler tick threw: "} + e.what());
        } catch (...) {
            self->logger_.error("MembershipReconciler tick threw unknown");
        }
        self->inFlight_.store(false, std::memory_order_release);
        co_return;
    }(this);
}

} // namespace aid::infrastructure
