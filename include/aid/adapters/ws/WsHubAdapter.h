#pragma once

#include <drogon/WebSocketConnection.h>

#include <cstddef>
#include <functional>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "aid/plumbing/ActionResult.h"
#include "aid/ports/UiNotifier.h"
#include "aid/value-types/Ids.h"

// In-process concrete UiNotifier. Tracks active WebSocket
// connections grouped by UserHandle and broadcasts invalidate / action-result
// frames. Not a .so plugin — Drogon types are allowed across this interface
// because callers (the UiStreamController + the use-cases that hold a
// UiNotifier& reference) live in the same process. Layering: depends on
// aid_ports + aid_drogon, but no use-case / controller / domain code.
//
// One mutex guards the subscriber map. Notify methods copy out the recipient
// connections under the lock and call send() outside it — Drogon's send is
// thread-safe (it dispatches onto the connection's I/O loop internally), so
// fanout from any thread is fine and we avoid holding our mutex during the
// framework-internal scheduling that could re-enter onDisconnect.
//
// Daemon-only header: it pulls drogon/WebSocketConnection.h for the
// WebSocketConnectionPtr in the subscriber map. Do not include from
// use-cases or domain — they speak the abstract UiNotifier port instead.

namespace aid::crosscutting {
class Logger;
} // namespace aid::crosscutting

namespace aid::adapters::ws {

class WsHubAdapter final : public aid::ports::UiNotifier {
public:
    // Hard cap. Enforcement is atomic inside
    // onConnect — the controller treats a false return as "refuse this
    // connection" and force-closes it. onConnect returns bool rather than
    // void so the cap check and the insert share a single mutex acquisition.
    static constexpr std::size_t MAX_SUBSCRIBERS = 500;

    explicit WsHubAdapter(aid::crosscutting::Logger& logger) noexcept;

    WsHubAdapter(const WsHubAdapter&) = delete;
    WsHubAdapter& operator=(const WsHubAdapter&) = delete;
    WsHubAdapter(WsHubAdapter&&) = delete;
    WsHubAdapter& operator=(WsHubAdapter&&) = delete;
    ~WsHubAdapter() override = default;

    [[nodiscard]] bool onConnect(aid::UserHandle viewer, drogon::WebSocketConnectionPtr conn);

    void onDisconnect(const drogon::WebSocketConnectionPtr& conn) noexcept;

    void notifyInvalidate(std::string_view scope) override;
    void notifyInvalidateUser(aid::UserHandle user, std::string_view scope) override;
    void notifyActionResult(aid::UserHandle user,
                            const aid::plumbing::ActionResult& result) override;
    void pushTicketUpsert(aid::UserHandle user, const aid::DashboardEntry& entry) override;
    void pushTicketRemove(aid::UserHandle user, aid::TicketId ticketId, int lockVersion) override;

    [[nodiscard]] std::size_t subscriberCount() const noexcept;

    // Cheap, in-memory "is ≥1 dashboard WebSocket connected right now?". The
    // MembershipReconciler consults this each tick so an idle daemon makes zero
    // membership round-trips. Just a guarded read of total_ — no fan-out, no
    // allocation. Bound into the reconciler as a ConnectionGate callable so
    // infrastructure does not depend on this adapter type.
    [[nodiscard]] bool anyConnected() const noexcept;

    // Connect-kick hook: invoked exactly once on each 0→1 subscriber transition
    // (the first dashboard to connect after the hub was empty). Main wires this
    // to MembershipReconciler::kick() so a freshly-arrived viewer gets an
    // immediate membership reconcile instead of waiting up to a full poll
    // interval. Fired OUTSIDE the mutex on the accepting WS loop thread; the
    // callback must be cheap and thread-safe (kick() just hops onto the domain
    // loop). Empty by default (no-op). Set once at startup, before listeners
    // open; not re-entrant-safe to swap under live traffic.
    void setOnFirstConnect(std::function<void()> cb) noexcept;

private:
    // Copy out one user's live connections under the lock, then send() each
    // outside it (Drogon's send is thread-safe; holding the mutex through the
    // framework-internal scheduling could re-enter onDisconnect). Shared by
    // every targeted frame: notifyInvalidateUser, notifyActionResult, and the
    // ticket_upsert / ticket_remove deltas.
    void sendToUser(const aid::UserHandle& user, std::string_view payload);

    aid::crosscutting::Logger& logger_;
    mutable std::mutex mtx_;
    std::unordered_map<aid::UserHandle, std::vector<drogon::WebSocketConnectionPtr>> subscribers_;
    std::size_t total_{0};
    // Set once at startup (before listeners open) and thereafter only read, so
    // it needs no synchronization with the onConnect loads. Default-empty.
    std::function<void()> onFirstConnect_;
};

} // namespace aid::adapters::ws
