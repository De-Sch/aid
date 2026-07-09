#include "aid/adapters/ws/WsHubAdapter.h"

#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "aid/crosscutting/Logger.h"
#include "aid/serialization/ActionResultJson.h"
#include "aid/serialization/DashboardJson.h"
#include "aid/value-types/Dashboard.h"

namespace aid::adapters::ws {

namespace {

using aid::crosscutting::LogType;

std::string makeInvalidatePayload(std::string_view scope) {
    nlohmann::json j;
    j["type"] = "invalidate";
    j["scope"] = std::string{scope};
    return j.dump();
}

std::string makeActionResultPayload(const aid::plumbing::ActionResult& r) {
    // Shared {ok, op, ticketId, message} projection (same rule the REST reply
    // uses), wrapped in the WS frame's type discriminator.
    nlohmann::json j = aid::serialization::toJson(r);
    j["type"] = "action_result";
    return j.dump();
}

std::string makeTicketUpsertPayload(const aid::DashboardEntry& entry) {
    nlohmann::json j;
    j["type"] = "ticket_upsert";
    j["entry"] = aid::serialization::toJson(entry);
    // lockVersion rides at the frame top level (not inside entry, which stays
    // byte-identical to the REST projection) so a viewer can drop a frame that
    // lost a race with a newer one for the same ticket.
    j["lockVersion"] = entry.lockVersion;
    return j.dump();
}

std::string makeTicketRemovePayload(const aid::TicketId& ticketId, int lockVersion) {
    nlohmann::json j;
    j["type"] = "ticket_remove";
    j["ticketId"] = ticketId.v;
    j["lockVersion"] = lockVersion;
    return j.dump();
}

void dispatch(const std::vector<drogon::WebSocketConnectionPtr>& conns, std::string_view payload) {
    for (const auto& c : conns) {
        if (c) {
            c->send(payload);
        }
    }
}

} // namespace

WsHubAdapter::WsHubAdapter(aid::crosscutting::Logger& logger) noexcept : logger_(logger) {
}

bool WsHubAdapter::onConnect(aid::UserHandle viewer, drogon::WebSocketConnectionPtr conn) {
    if (!conn) {
        return false;
    }
    bool firstConnect = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        if (total_ >= MAX_SUBSCRIBERS) {
            logger_.warn("WsHub: 500-connection cap reached, refusing onConnect",
                         LogType::FRONTEND);
            return false;
        }
        subscribers_[viewer].push_back(std::move(conn));
        firstConnect = (total_ == 0);
        ++total_;
    }
    // Fire the connect-kick OUTSIDE the mutex: kick() may queue onto another
    // loop and we must not hold mtx_ across framework-internal scheduling (same
    // reasoning as the send-outside-the-lock pattern). Only on the genuine 0→1
    // transition, and only if Main wired a handler.
    if (firstConnect && onFirstConnect_) {
        onFirstConnect_();
    }
    return true;
}

void WsHubAdapter::onDisconnect(const drogon::WebSocketConnectionPtr& conn) noexcept {
    if (!conn) {
        return;
    }
    std::lock_guard<std::mutex> lk(mtx_);
    for (auto it = subscribers_.begin(); it != subscribers_.end();) {
        auto& vec = it->second;
        const std::size_t removed = std::erase(vec, conn);
        total_ -= removed;
        if (removed > 0 && vec.empty()) {
            it = subscribers_.erase(it);
        } else {
            ++it;
        }
    }
}

void WsHubAdapter::notifyInvalidate(std::string_view scope) {
    const std::string payload = makeInvalidatePayload(scope);
    std::vector<drogon::WebSocketConnectionPtr> recipients;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        recipients.reserve(total_);
        for (const auto& [_, v] : subscribers_) {
            recipients.insert(recipients.end(), v.begin(), v.end());
        }
    }
    dispatch(recipients, payload);
}

void WsHubAdapter::sendToUser(const aid::UserHandle& user, std::string_view payload) {
    std::vector<drogon::WebSocketConnectionPtr> recipients;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        const auto it = subscribers_.find(user);
        if (it != subscribers_.end()) {
            recipients = it->second;
        }
    }
    dispatch(recipients, payload);
}

void WsHubAdapter::notifyInvalidateUser(aid::UserHandle user, std::string_view scope) {
    sendToUser(user, makeInvalidatePayload(scope));
}

void WsHubAdapter::notifyActionResult(aid::UserHandle user,
                                      const aid::plumbing::ActionResult& result) {
    sendToUser(user, makeActionResultPayload(result));
}

void WsHubAdapter::pushTicketUpsert(aid::UserHandle user, const aid::DashboardEntry& entry) {
    sendToUser(user, makeTicketUpsertPayload(entry));
}

void WsHubAdapter::pushTicketRemove(aid::UserHandle user, aid::TicketId ticketId, int lockVersion) {
    sendToUser(user, makeTicketRemovePayload(ticketId, lockVersion));
}

std::size_t WsHubAdapter::subscriberCount() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return total_;
}

bool WsHubAdapter::anyConnected() const noexcept {
    std::lock_guard<std::mutex> lk(mtx_);
    return total_ > 0;
}

void WsHubAdapter::setOnFirstConnect(std::function<void()> cb) noexcept {
    onFirstConnect_ = std::move(cb);
}

} // namespace aid::adapters::ws
