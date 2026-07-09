#include "aid/infrastructure/MailboxEngine.h"

#include <trantor/net/EventLoop.h>

#include <cassert>
#include <future>
#include <string_view>
#include <thread>
#include <utility>

#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Wal.h"
#include "aid/plumbing/Error.h"
#include "aid/value-types/CallEvent.h"
#include "aid/value-types/Ids.h"

namespace aid::infrastructure {

namespace {

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;

Error rejection(const std::string& msg, std::string_view cid = {}) {
    Error e{ErrorCode::InvalidInput, msg, std::nullopt};
    if (!cid.empty()) {
        e.correlationId = std::string{cid};
    }
    return e;
}

} // namespace

template <class Key, class Payload>
MailboxEngine<Key, Payload>::MailboxEngine(trantor::EventLoop& domainLoop, Wal& wal,
                                           aid::crosscutting::Logger& logger, Dispatch dispatch,
                                           Labels labels)
    : domainLoop_(domainLoop), wal_(wal), logger_(logger), dispatch_(std::move(dispatch)),
      labels_(std::move(labels)) {
}

template <class Key, class Payload> MailboxEngine<Key, Payload>::~MailboxEngine() {
    // Precondition the caller must uphold: `drain(budget)` has returned and
    // no further calls to `enqueue` / `enqueueBypass` are in flight on any
    // thread. The dtor cannot block enqueues from threads already past the
    // `draining_` check — that race is the caller's to prevent (typically
    // by quiescing the controller listeners before tearing down the facade).
    //
    // Given that precondition, the only callbacks still pending on the
    // domain loop are (a) spawnWorker lambdas queued from past enqueues
    // that haven't fired yet, and (b) eraseInFlight lambdas queued by
    // workers reaching final_suspend. Two barriers drain them: barrier 1
    // processes the spawns (each runs its worker synchronously, since
    // there is no co_await in handlers yet, and queues its own
    // eraseInFlight *after* barrier 1); barrier 2 processes those
    // eraseInFlight tasks. After barrier 2 fires, inFlight_ holds only
    // Task<void>s suspended at final_suspend — destroying them is the
    // documented-safe state for std::coroutine_handle::destroy().
    //
    // The wait is bounded so a non-running loop (e.g. a test that
    // constructs an EventLoop but never calls loop()) does not hang the
    // dtor. On timeout, the queued barrier lambda is never invoked but
    // owns the promise via shared_ptr; if the lambda is later destroyed
    // by ~EventLoop, the promise is released cleanly.
    draining_.store(true, std::memory_order_release);
    for (int i = 0; i < 2; ++i) {
        auto barrier = std::make_shared<std::promise<void>>();
        auto fut = barrier->get_future();
        domainLoop_.queueInLoop([barrier] { barrier->set_value(); });
        if (fut.wait_for(std::chrono::seconds(2)) != std::future_status::ready) {
            break;
        }
    }
}

template <class Key, class Payload>
aid::plumbing::Result<void>
MailboxEngine<Key, Payload>::enqueue(Key key, Payload payload, std::string correlationId,
                                     std::uint64_t walSeq, bool replay) {
    bool needSpawn = false;
    {
        std::lock_guard lk{mtx_};
        if (draining_.load(std::memory_order_acquire)) {
            return aid::plumbing::unexpected{rejection("draining", correlationId)};
        }
        const bool isNewKey = !queues_.contains(key);
        if (isNewKey && queues_.size() >= MAX_LIVE_MAILBOXES) {
            return aid::plumbing::unexpected{
                rejection(labels_.prefix + " cap reached", correlationId)};
        }
        auto& dq = queues_[key];
        if (dq.size() >= MAX_QUEUE) {
            return aid::plumbing::unexpected{rejection(labels_.prefix + " full", correlationId)};
        }
        dq.push_back(Pending{walSeq, std::move(payload), correlationId, replay});
        lastActivity_[key] = std::chrono::steady_clock::now();
        needSpawn = activeWorkers_.insert(key).second;
    }
    if (needSpawn) {
        domainLoop_.queueInLoop([this, k = key]() { spawnWorker(k); });
    }
    return {};
}

template <class Key, class Payload>
void MailboxEngine<Key, Payload>::enqueueBypass(Key key, Payload payload, std::string correlationId,
                                                std::uint64_t walSeq, bool replay) {
    bool needSpawn = false;
    {
        std::lock_guard lk{mtx_};
        auto& dq = queues_[key];
        dq.push_back(Pending{walSeq, std::move(payload), std::move(correlationId), replay});
        lastActivity_[key] = std::chrono::steady_clock::now();
        needSpawn = activeWorkers_.insert(key).second;
    }
    if (needSpawn) {
        domainLoop_.queueInLoop([this, k = key]() { spawnWorker(k); });
    }
}

template <class Key, class Payload> std::size_t MailboxEngine<Key, Payload>::liveCount() const {
    std::lock_guard lk{mtx_};
    std::size_t total = 0;
    for (const auto& kv : queues_) {
        total += kv.second.size();
    }
    return total;
}

template <class Key, class Payload>
std::size_t MailboxEngine<Key, Payload>::failedCount() const noexcept {
    return failedCount_.load(std::memory_order_acquire);
}

template <class Key, class Payload>
std::size_t MailboxEngine<Key, Payload>::trackedMailboxCount() const {
    std::lock_guard lk{mtx_};
    return queues_.size();
}

template <class Key, class Payload>
void MailboxEngine<Key, Payload>::gcIdleOlderThan(std::chrono::seconds idle) {
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lk{mtx_};
    for (auto it = lastActivity_.begin(); it != lastActivity_.end();) {
        const auto& key = it->first;
        const bool active = activeWorkers_.contains(key);
        const auto qIt = queues_.find(key);
        const bool empty = qIt == queues_.end() || qIt->second.empty();
        if (!active && empty && now - it->second > idle) {
            if (qIt != queues_.end()) {
                queues_.erase(qIt);
            }
            it = lastActivity_.erase(it);
        } else {
            ++it;
        }
    }
}

template <class Key, class Payload>
aid::plumbing::Task<void> MailboxEngine<Key, Payload>::drain(std::chrono::seconds budget) {
    draining_.store(true, std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + budget;
    while (std::chrono::steady_clock::now() < deadline) {
        bool empty = false;
        {
            std::lock_guard lk{mtx_};
            empty = activeWorkers_.empty();
        }
        if (empty) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    co_return;
}

template <class Key, class Payload> void MailboxEngine<Key, Payload>::spawnWorker(Key key) {
    // On the domain loop. The eager-started Task<void> stays in inFlight_
    // until the worker schedules its own erasure at end-of-life. Without
    // this storage, the Task<void> dtor would run h_.destroy() on the
    // suspended frame after the first co_await — a use-after-free for
    // anything awaiting the worker.
    //
    // A previous worker's entry can linger here if its queued eraseInFlight
    // task has not yet been processed by the loop. That entry is always at
    // final_suspend by the time we get here (the previous worker's spawn
    // task ran to completion before this one was dispatched), so erasing
    // it is safe; Task<void> is move-only with assignment deleted, hence
    // erase-then-emplace rather than insert_or_assign.
    //
    // That "always at final_suspend" claim is a
    // load-bearing timing invariant only ever exercised after the 1 h GC
    // cutoff. Assert it so a future regression (e.g. a re-spawn before the
    // prior worker's deferred eraseInFlight ran) fails loud instead of
    // destroying a live frame in erase() below.
#ifndef NDEBUG
    const auto existing = inFlight_.find(key);
    assert((existing == inFlight_.end() || existing->second.done()) &&
           "spawnWorker: previous worker frame must be at final_suspend before re-spawn");
#endif
    inFlight_.erase(key);
    auto task = workerCoroutine(key);
    inFlight_.emplace(std::move(key), std::move(task));
}

template <class Key, class Payload>
aid::plumbing::Task<void> MailboxEngine<Key, Payload>::workerCoroutine(Key key) {
    try {
        while (true) {
            Pending p;
            {
                std::lock_guard lk{mtx_};
                auto it = queues_.find(key);
                if (it == queues_.end() || it->second.empty()) {
                    activeWorkers_.erase(key);
                    break;
                }
                p = std::move(it->second.front());
                it->second.pop_front();
            }

            // Top-level try-catch (below). The dispatch may throw or
            // return an Error; both must leave the WAL record in place for
            // replay.
            auto r = co_await dispatch_(p);
            if (r) {
                const auto acked = wal_.ack(p.walSeq);
                if (!acked) {
                    failedCount_.fetch_add(1, std::memory_order_release);
                    logger_.error(labels_.prefix + ": WAL ack failed: " + acked.error().message,
                                  aid::crosscutting::LogType::BACKEND,
                                  std::string_view{p.correlationId});
                } else {
                    logger_.debug(labels_.prefix + ": " + labels_.handledLabel + "=" + key.v +
                                      " seq=" + std::to_string(p.walSeq),
                                  aid::crosscutting::LogType::BACKEND,
                                  std::string_view{p.correlationId});
                }
            } else {
                failedCount_.fetch_add(1, std::memory_order_release);
                logger_.error(labels_.prefix + ": " + labels_.failLabel + ": " + r.error().message,
                              aid::crosscutting::LogType::BACKEND,
                              std::string_view{p.correlationId});
                // Leave WAL record in place — operator can replay after fix.
            }
        }
    } catch (const std::exception& e) {
        failedCount_.fetch_add(1, std::memory_order_release);
        std::lock_guard lk{mtx_};
        activeWorkers_.erase(key);
        logger_.error(labels_.prefix + " worker threw: " + e.what());
    } catch (...) {
        failedCount_.fetch_add(1, std::memory_order_release);
        std::lock_guard lk{mtx_};
        activeWorkers_.erase(key);
        logger_.error(labels_.prefix + " worker threw unknown");
    }

    // Self-erasure must be deferred: destroying inFlight_[key] here
    // would destroy the frame we're still suspended in.
    domainLoop_.queueInLoop([this, key]() { inFlight_.erase(key); });
    co_return;
}

// Explicit instantiations — the closed set of payload/key pairs. Keeping the
// template body in this single TU (rather than the header) means the delicate
// coroutine/lifetime code compiles exactly once and lives in one place.
template class MailboxEngine<aid::CallId, aid::CallEvent>;
template class MailboxEngine<aid::TicketId, std::string>;

} // namespace aid::infrastructure
