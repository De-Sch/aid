#include "aid/controllers/WebhookController.h"

#include <cstddef>
#include <exception>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>
#include <utility>

#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Wal.h"
#include "aid/infrastructure/WebhookMailbox.h"
#include "aid/value-types/Ids.h"

namespace aid::controllers {

namespace {

using aid::crosscutting::Logger;
using aid::crosscutting::LogType;

drogon::HttpResponsePtr respond(drogon::HttpStatusCode code) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    return resp;
}

// Constant-time string compare — never short-circuits on the first mismatching
// byte, so a network attacker cannot recover the secret one character at a time
// from response timing. Differing lengths always fail (but still scan `a` so the
// loop cost does not leak the configured length).
bool constantTimeEquals(std::string_view a, std::string_view b) {
    unsigned char diff = a.size() == b.size() ? 0u : 1u;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const unsigned char bc = i < b.size() ? static_cast<unsigned char>(b[i]) : 0u;
        diff |= static_cast<unsigned char>(static_cast<unsigned char>(a[i]) ^ bc);
    }
    return diff == 0;
}

} // namespace

std::optional<aid::TicketId> WebhookController::ticketIdOf(std::string_view body) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!j.is_object()) {
        return std::nullopt;
    }
    // Prefer the envelope's work_package object; fall back to a bare HAL body.
    const nlohmann::json* wp = &j;
    if (auto it = j.find("work_package"); it != j.end() && it->is_object()) {
        wp = &*it;
    }
    auto idIt = wp->find("id");
    if (idIt == wp->end()) {
        return std::nullopt;
    }
    if (idIt->is_number_integer()) {
        return aid::TicketId{std::to_string(idIt->get<long long>())};
    }
    if (idIt->is_string()) {
        auto s = idIt->get<std::string>();
        if (s.empty()) {
            return std::nullopt;
        }
        return aid::TicketId{std::move(s)};
    }
    return std::nullopt;
}

WebhookController::WebhookController(aid::infrastructure::Wal& wal,
                                     aid::infrastructure::WebhookMailbox& mailbox,
                                     aid::crosscutting::Logger& logger,
                                     aid::crosscutting::CorrelationId& cid, std::string secret)
    : wal_(wal), mailbox_(mailbox), logger_(logger), cid_(cid), secret_(std::move(secret)) {
}

void WebhookController::handlePost(const drogon::HttpRequestPtr& req,
                                   std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    const std::string cidStr = cid_.nextUuid();

    // Shared-secret gate. The ticket system can attach the secret either as a custom
    // header on the payload request or as a query parameter on the configured
    // payload URL; accept both. Constant-time compare. The secret itself is
    // never logged.
    std::string presented = req->getHeader("X-AID-Webhook-Secret");
    if (presented.empty()) {
        presented = req->getParameter("secret");
    }
    if (secret_.empty() || !constantTimeEquals(secret_, presented)) {
        logger_.warn("WebhookController: rejected request with bad/absent secret", LogType::BACKEND,
                     std::string_view{cidStr});
        callback(respond(drogon::k401Unauthorized));
        return;
    }

    // Copy the body before WAL append / enqueue — getBody()'s view is
    // invalid after this handler returns.
    const std::string body{req->getBody()};

    auto ticketId = ticketIdOf(body);
    if (!ticketId) {
        logger_.warn("WebhookController: could not extract ticket id from body", LogType::BACKEND,
                     std::string_view{cidStr});
        callback(respond(drogon::k400BadRequest));
        return;
    }

    auto seqRes = wal_.append(body, cidStr);
    if (!seqRes) {
        logger_.error(std::string{"WebhookController: WAL append failed: "} +
                          seqRes.error().message,
                      LogType::BACKEND, std::string_view{cidStr});
        callback(respond(drogon::k500InternalServerError));
        return;
    }

    auto enq = mailbox_.enqueue(*ticketId, body, cidStr, *seqRes);
    if (!enq) {
        // Backpressure: queue full / cap reached / draining → 503.
        logger_.error(std::string{"WebhookController: mailbox rejected enqueue: "} +
                          enq.error().message,
                      LogType::BACKEND, std::string_view{cidStr});
        callback(respond(drogon::k503ServiceUnavailable));
        return;
    }

    logger_.debug("WebhookController: accepted ticket=" + ticketId->v +
                      " seq=" + std::to_string(*seqRes),
                  LogType::BACKEND, std::string_view{cidStr});
    callback(respond(drogon::k202Accepted));
}

} // namespace aid::controllers
