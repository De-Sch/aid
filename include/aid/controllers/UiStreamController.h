#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <drogon/WebSocketController.h>

#include <string>

// Drogon WebSocket controller for /ui/stream. Accepts the WS
// upgrade behind SessionGuard, registers the connection with WsHubAdapter,
// and deregisters on close. Server-push only — incoming frames are ignored.
//
// Drogon constructs WebSocketControllers through DrObject reflection (default
// ctor only), so the hub / logger / cid dependencies are bound once at startup
// via the static install() helper. The plain UiController/LoginController/
// CallController pattern of constructor-DI + main-side lambda registration is
// not available here: Drogon's WebSocketController class is the only path
// to /ui/stream-style routing in v1.9.13.
//
// Tests call install() in their fixture, instantiate the controller directly,
// drive the three handle*() methods, and call uninstall() in tear-down.

namespace aid::adapters::ws {
class WsHubAdapter;
} // namespace aid::adapters::ws

namespace aid::crosscutting {
class CorrelationId;
class Logger;
} // namespace aid::crosscutting

namespace aid::controllers {

class UiStreamController final
    : public drogon::WebSocketController<UiStreamController, /*AutoCreation=*/true> {
public:
    UiStreamController() = default;

    UiStreamController(const UiStreamController&) = delete;
    UiStreamController& operator=(const UiStreamController&) = delete;
    UiStreamController(UiStreamController&&) = delete;
    UiStreamController& operator=(UiStreamController&&) = delete;
    ~UiStreamController() override = default;

    // Bind shared deps. Must be called exactly once at startup, before
    // app().run() opens the LAN listener. Subsequent calls overwrite — tests
    // exploit this to swap deps between fixtures; production calls it once.
    static void install(aid::adapters::ws::WsHubAdapter& hub, aid::crosscutting::Logger& logger,
                        aid::crosscutting::CorrelationId& cid) noexcept;

    // Reset bound deps (tests only). Production never invokes this.
    static void uninstall() noexcept;

    WS_PATH_LIST_BEGIN
    WS_PATH_ADD("/ui/stream", "aid::controllers::SessionGuard");
    WS_PATH_LIST_END

    void handleNewConnection(const drogon::HttpRequestPtr& req,
                             const drogon::WebSocketConnectionPtr& conn) override;

    void handleConnectionClosed(const drogon::WebSocketConnectionPtr& conn) override;

    void handleNewMessage(const drogon::WebSocketConnectionPtr& conn, std::string&& message,
                          const drogon::WebSocketMessageType& type) override;
};

} // namespace aid::controllers
