#pragma once

#include <drogon/HttpTypes.h>
#include <drogon/WebSocketConnection.h>
#include <json/value.h>
#include <trantor/net/InetAddress.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

// Minimal drogon::WebSocketConnection test double. Records text sent via
// send(string_view) and tracks open/closed flags. The other 9 pure virtuals
// are stubbed: the hub never calls them, but Drogon still requires them to
// be implemented so the class is instantiable.

namespace aid::fakes {

class FakeWebSocketConnection final : public drogon::WebSocketConnection {
public:
    FakeWebSocketConnection() = default;

    FakeWebSocketConnection(const FakeWebSocketConnection&) = delete;
    FakeWebSocketConnection& operator=(const FakeWebSocketConnection&) = delete;
    FakeWebSocketConnection(FakeWebSocketConnection&&) = delete;
    FakeWebSocketConnection& operator=(FakeWebSocketConnection&&) = delete;
    ~FakeWebSocketConnection() override = default;

    void send(const char* msg, uint64_t len,
              drogon::WebSocketMessageType type = drogon::WebSocketMessageType::Text) override;

    void send(std::string_view msg,
              drogon::WebSocketMessageType type = drogon::WebSocketMessageType::Text) override;

    void sendJson(const Json::Value& json,
                  drogon::WebSocketMessageType type = drogon::WebSocketMessageType::Text) override;

    [[nodiscard]] const trantor::InetAddress& localAddr() const override;
    [[nodiscard]] const trantor::InetAddress& peerAddr() const override;

    [[nodiscard]] bool connected() const override;
    [[nodiscard]] bool disconnected() const override;

    void shutdown(drogon::CloseCode code = drogon::CloseCode::kNormalClosure,
                  const std::string& reason = "") override;

    void forceClose() override;

    void setPingMessage(const std::string& message,
                        const std::chrono::duration<double>& interval) override;

    void disablePing() override;

    // Test observation API.
    [[nodiscard]] std::vector<std::string> sent() const;
    [[nodiscard]] std::size_t sentCount() const;
    [[nodiscard]] bool isClosed() const noexcept;

private:
    mutable std::mutex mtx_;
    std::vector<std::string> sent_;
    std::atomic<bool> closed_{false};
    std::atomic<bool> connected_{true};
    trantor::InetAddress dummyAddr_{};
};

} // namespace aid::fakes
