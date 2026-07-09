#include "aid/controllers/UiStreamController.h"

#include <atomic>

#include "aid/adapters/ws/WsHubAdapter.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/value-types/Ids.h"

namespace aid::controllers {

namespace {

using aid::crosscutting::LogType;

struct Deps {
    aid::adapters::ws::WsHubAdapter* hub;
    aid::crosscutting::Logger* logger;
    aid::crosscutting::CorrelationId* cid;
};

// One indirection through a static Deps bag, so install() publishes the
// triple atomically. Drogon constructs WebSocketControllers via DrObject
// reflection (default ctor only), so per-instance member injection isn't
// available — but at least a re-install during tests can never publish a
// torn (hub-set, logger-still-null) snapshot. Default seq_cst ordering is
// fine: install runs once at startup, fixture swaps are infrequent, and
// the loads happen on the I/O loop where cache lines are hot anyway.
Deps g_deps_storage{};
std::atomic<Deps*> g_deps{nullptr};

} // namespace

void UiStreamController::install(aid::adapters::ws::WsHubAdapter& hub,
                                 aid::crosscutting::Logger& logger,
                                 aid::crosscutting::CorrelationId& cid) noexcept {
    g_deps_storage = Deps{&hub, &logger, &cid};
    g_deps.store(&g_deps_storage);
}

void UiStreamController::uninstall() noexcept {
    g_deps.store(nullptr);
    g_deps_storage = Deps{};
}

void UiStreamController::handleNewConnection(const drogon::HttpRequestPtr& req,
                                             const drogon::WebSocketConnectionPtr& conn) {
    auto* deps = g_deps.load();
    if (deps == nullptr) {
        // Bootstrap defect: install() was never called. No logger to use.
        if (conn) {
            conn->forceClose();
        }
        return;
    }
    auto& hub = *deps->hub;
    auto& logger = *deps->logger;
    auto& cid = *deps->cid;

    const std::string cidStr = cid.nextUuid();

    if (!req || !req->attributes()->find(SessionGuard::VIEWER_KEY)) {
        logger.fatal("UiStream: handshake reached without viewer attribute "
                     "(SessionGuard filter misconfigured)",
                     LogType::FRONTEND, cidStr);
        if (conn) {
            conn->forceClose();
        }
        return;
    }

    const auto viewer = req->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
    if (viewer.v.empty()) {
        logger.fatal("UiStream: empty viewer handle in handshake attribute", LogType::FRONTEND,
                     cidStr);
        if (conn) {
            conn->forceClose();
        }
        return;
    }

    if (!hub.onConnect(viewer, conn)) {
        logger.warn("UiStream: refusing /ui/stream upgrade — 500-connection cap reached",
                    LogType::FRONTEND, cidStr);
        if (conn) {
            conn->forceClose();
        }
        return;
    }

    logger.info("UiStream: new subscriber for user=" + viewer.v, LogType::FRONTEND, cidStr);
}

void UiStreamController::handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) {
    auto* deps = g_deps.load();
    if (deps == nullptr) {
        return;
    }
    deps->hub->onDisconnect(conn);
    deps->logger->info("UiStream: subscriber departed", LogType::FRONTEND);
}

void UiStreamController::handleNewMessage(const drogon::WebSocketConnectionPtr&, std::string&&,
                                          const drogon::WebSocketMessageType&) {
    // /ui/stream is server-push only; inbound frames are ignored per spec.
}

} // namespace aid::controllers
