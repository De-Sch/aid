#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

// HealthService. Runs a one-shot upstream probe at
// startup, caches the result, and combines it with live counters from
// Mailbox + a steady_clock uptime on every /health read. Non-blocking
// from the caller's perspective — the cached snapshot is set once before
// drogon::app().run() returns and never re-pinged in v1 (a periodic
// probe is explicitly out of scope).
//
// Translation note: the spec sketches `std::atomic<Snapshot>` but
// Snapshot holds std::string, so it is not trivially-copyable. We hold
// `std::atomic<std::shared_ptr<const Snapshot>>` (C++20) — semantically
// equivalent: one writer at boot, many lock-free readers from the
// listener threads.

namespace aid::ports {
class TicketStore;
class AddressBook;
} // namespace aid::ports

namespace aid::infrastructure {

class Mailbox;

class HealthService {
public:
    struct Snapshot {
        std::string status; // "ok" | "degraded" | "starting"
        bool plugins_loaded = false;
        std::string ticket_system;  // "reachable" | "unreachable"
        std::string address_system; // "reachable" | "unreachable"
        std::int64_t uptime_s = 0;
        std::size_t queued_events = 0;
        std::size_t failed_events = 0;
    };

    HealthService(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab, Mailbox& mailbox,
                  bool pluginsLoaded);

    HealthService(const HealthService&) = delete;
    HealthService& operator=(const HealthService&) = delete;
    HealthService(HealthService&&) = delete;
    HealthService& operator=(HealthService&&) = delete;
    ~HealthService() = default;

    // Cold-start probe. Called from Main BEFORE
    // drogon::app().run() so /health is meaningful from second 1.
    // Probes both upstreams once; sets the cached Snapshot accordingly.
    // Never throws (each port's ping() must return Result<void>).
    [[nodiscard]] aid::plumbing::Task<void> bootstrapPing();

    // Non-blocking read. Copies the cached upstream-status fields and
    // fills in uptime_s, queued_events, failed_events from live sources
    // (steady_clock + Mailbox accessors).
    //
    // failed_events is delegated to Mailbox::failedCount() rather than a
    // HealthService-owned counter (deliberate spec deviation — Mailbox
    // already owns the increment sites for retry exhaustion / truncate
    // failures, so a HealthService-owned mirror would just double-book).
    [[nodiscard]] Snapshot current() const;

private:
    aid::ports::TicketStore& ts_;
    aid::ports::AddressBook& ab_;
    Mailbox& mailbox_;
    bool pluginsLoaded_;
    std::chrono::steady_clock::time_point startedAt_;
    // REASON: C++20 partial specialization (libstdc++ 12+). One writer at
    // boot, many readers from listener threads — pointer swap is lock-free
    // on x86_64, payload is immutable so readers never race the writer.
    std::atomic<std::shared_ptr<const Snapshot>> snap_;
};

} // namespace aid::infrastructure
