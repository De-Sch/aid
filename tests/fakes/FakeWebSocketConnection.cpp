#include "FakeWebSocketConnection.h"

namespace aid::fakes {

void FakeWebSocketConnection::send(const char* msg, uint64_t len, drogon::WebSocketMessageType) {
    std::lock_guard<std::mutex> lk(mtx_);
    sent_.emplace_back(msg, msg + len);
}

void FakeWebSocketConnection::send(std::string_view msg, drogon::WebSocketMessageType) {
    std::lock_guard<std::mutex> lk(mtx_);
    sent_.emplace_back(msg);
}

void FakeWebSocketConnection::sendJson(const Json::Value& json, drogon::WebSocketMessageType) {
    std::lock_guard<std::mutex> lk(mtx_);
    sent_.emplace_back(json.toStyledString());
}

const trantor::InetAddress& FakeWebSocketConnection::localAddr() const {
    return dummyAddr_;
}

const trantor::InetAddress& FakeWebSocketConnection::peerAddr() const {
    return dummyAddr_;
}

bool FakeWebSocketConnection::connected() const {
    return connected_.load();
}

bool FakeWebSocketConnection::disconnected() const {
    return !connected_.load();
}

void FakeWebSocketConnection::shutdown(drogon::CloseCode, const std::string&) {
    connected_.store(false);
    closed_.store(true);
}

void FakeWebSocketConnection::forceClose() {
    connected_.store(false);
    closed_.store(true);
}

void FakeWebSocketConnection::setPingMessage(const std::string&,
                                             const std::chrono::duration<double>&) {
}

void FakeWebSocketConnection::disablePing() {
}

std::vector<std::string> FakeWebSocketConnection::sent() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sent_;
}

std::size_t FakeWebSocketConnection::sentCount() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return sent_.size();
}

bool FakeWebSocketConnection::isClosed() const noexcept {
    return closed_.load();
}

} // namespace aid::fakes
