#include "aid/infrastructure/HealthService.h"

#include <chrono>
#include <memory>
#include <utility>

#include "aid/infrastructure/Mailbox.h"
#include "aid/ports/AddressBook.h"
#include "aid/ports/TicketStore.h"

namespace aid::infrastructure {

namespace {

constexpr const char* kReachable = "reachable";
constexpr const char* kUnreachable = "unreachable";
constexpr const char* kOk = "ok";
constexpr const char* kDegraded = "degraded";
constexpr const char* kStarting = "starting";

} // namespace

HealthService::HealthService(aid::ports::TicketStore& ts, aid::ports::AddressBook& ab,
                             Mailbox& mailbox, bool pluginsLoaded)
    : ts_(ts), ab_(ab), mailbox_(mailbox), pluginsLoaded_(pluginsLoaded),
      startedAt_(std::chrono::steady_clock::now()) {
    // Pre-bootstrap snapshot: status "starting", both upstreams unreachable
    // until bootstrapPing() proves otherwise. Guarantees a non-null pointer
    // for current() even if /health is hit before bootstrapPing() returns
    // (shouldn't happen — Main awaits the ping before app.run() — but the
    // invariant is cheap to uphold).
    auto initial = std::make_shared<const Snapshot>(
        Snapshot{kStarting, pluginsLoaded_, kUnreachable, kUnreachable, 0, 0, 0});
    snap_.store(std::move(initial), std::memory_order_release);
}

aid::plumbing::Task<void> HealthService::bootstrapPing() {
    auto opResult = co_await ts_.ping();
    auto dbResult = co_await ab_.ping();

    const bool opOk = opResult.has_value();
    const bool dbOk = dbResult.has_value();

    auto next = std::make_shared<const Snapshot>(
        Snapshot{(opOk && dbOk) ? kOk : kDegraded, pluginsLoaded_, opOk ? kReachable : kUnreachable,
                 dbOk ? kReachable : kUnreachable, 0, 0, 0});
    snap_.store(std::move(next), std::memory_order_release);
    co_return;
}

HealthService::Snapshot HealthService::current() const {
    auto snap = snap_.load(std::memory_order_acquire);
    Snapshot out = *snap;
    const auto elapsed = std::chrono::steady_clock::now() - startedAt_;
    out.uptime_s = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    out.queued_events = mailbox_.liveCount();
    out.failed_events = mailbox_.failedCount();
    return out;
}

} // namespace aid::infrastructure
