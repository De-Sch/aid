#include "aid/controllers/CallController.h"

#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/infrastructure/Mailbox.h"
#include "aid/infrastructure/Wal.h"
#include "aid/value-types/CallEvent.h"
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

std::optional<std::string> stringField(const nlohmann::json& j, const char* key) {
    auto it = j.find(key);
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

std::optional<aid::CallEvent> decodeIncoming(const nlohmann::json& j) {
    auto callid = stringField(j, "callid");
    auto remote = stringField(j, "remote");
    auto dialed = stringField(j, "dialed");
    if (!callid || !remote || !dialed) {
        return std::nullopt;
    }
    return aid::IncomingCall{aid::CallId{std::move(*callid)}, aid::PhoneNumber{std::move(*remote)},
                             aid::PhoneNumber{std::move(*dialed)}};
}

std::optional<aid::CallEvent> decodeAccepted(const nlohmann::json& j) {
    auto callid = stringField(j, "callid");
    auto remote = stringField(j, "remote");
    auto dialed = stringField(j, "dialed");
    if (!callid || !remote || !dialed) {
        return std::nullopt;
    }
    aid::AcceptedCall ev;
    ev.callid = aid::CallId{std::move(*callid)};
    ev.remote = aid::PhoneNumber{std::move(*remote)};
    ev.dialed = aid::PhoneNumber{std::move(*dialed)};
    // user is optional — absent when ConnectedLineName == "<unknown>"
    if (auto user = stringField(j, "user")) {
        ev.user = aid::UserHandle{std::move(*user)};
    }
    return ev;
}

std::optional<aid::CallEvent> decodeOutgoing(const nlohmann::json& j) {
    // Outgoing has NO `dialed` field per calls.py line 40.
    auto callid = stringField(j, "callid");
    auto remote = stringField(j, "remote");
    auto user = stringField(j, "user");
    if (!callid || !remote || !user) {
        return std::nullopt;
    }
    return aid::OutgoingCall{aid::CallId{std::move(*callid)}, aid::PhoneNumber{std::move(*remote)},
                             aid::UserHandle{std::move(*user)}};
}

std::optional<aid::CallEvent> decodeTransfer(const nlohmann::json& j) {
    // Transfer uses field name `newuser`, not `user`, per calls.py line 43.
    auto callid = stringField(j, "callid");
    auto newuser = stringField(j, "newuser");
    if (!callid || !newuser) {
        return std::nullopt;
    }
    return aid::TransferCall{aid::CallId{std::move(*callid)}, aid::UserHandle{std::move(*newuser)}};
}

std::optional<aid::CallEvent> decodeHangup(const nlohmann::json& j) {
    auto callid = stringField(j, "callid");
    auto remote = stringField(j, "remote");
    if (!callid || !remote) {
        return std::nullopt;
    }
    return aid::HangupCall{aid::CallId{std::move(*callid)}, aid::PhoneNumber{std::move(*remote)}};
}

} // namespace

std::optional<aid::CallEvent> CallController::decodeJson(std::string_view body) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const std::exception&) {
        return std::nullopt;
    }
    auto ev = stringField(j, "event");
    if (!ev) {
        return std::nullopt;
    }
    if (*ev == "Incoming Call") {
        return decodeIncoming(j);
    }
    if (*ev == "Accepted Call") {
        return decodeAccepted(j);
    }
    if (*ev == "Outgoing Call") {
        return decodeOutgoing(j);
    }
    if (*ev == "Transfer Call") {
        return decodeTransfer(j);
    }
    if (*ev == "Hangup") {
        return decodeHangup(j);
    }
    return std::nullopt;
}

CallController::CallController(aid::infrastructure::Wal& wal, aid::infrastructure::Mailbox& mailbox,
                               aid::crosscutting::Logger& logger,
                               aid::crosscutting::CorrelationId& cid)
    : wal_(wal), mailbox_(mailbox), logger_(logger), cid_(cid) {
}

void CallController::handlePost(const drogon::HttpRequestPtr& req,
                                std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
    // Copy body now — getBody()'s string_view is invalid after this
    // handler returns, and Wal::append + Mailbox::enqueue both keep the
    // bytes.
    const std::string body{req->getBody()};
    const std::string cidStr = cid_.nextUuid();

    auto eventOpt = decodeJson(body);
    if (!eventOpt) {
        logger_.warn(std::string{"CallController: decode failed for body: "} + body,
                     LogType::BACKEND, std::string_view{cidStr});
        callback(respond(drogon::k400BadRequest));
        return;
    }

    auto seqRes = wal_.append(body, cidStr);
    if (!seqRes) {
        logger_.error(std::string{"CallController: WAL append failed: "} + seqRes.error().message,
                      LogType::BACKEND, std::string_view{cidStr});
        callback(respond(drogon::k500InternalServerError));
        return;
    }

    const auto callid = aid::callidOf(*eventOpt);
    auto enq = mailbox_.enqueue(callid, std::move(*eventOpt), cidStr, *seqRes);
    if (!enq) {
        // Backpressure: queue full / cap reached / draining -> 503.
        logger_.error(std::string{"CallController: mailbox rejected enqueue: "} +
                          enq.error().message,
                      LogType::BACKEND, std::string_view{cidStr});
        callback(respond(drogon::k503ServiceUnavailable));
        return;
    }

    logger_.debug("CallController: accepted callid=" + callid.v + " seq=" + std::to_string(*seqRes),
                  LogType::BACKEND, std::string_view{cidStr});
    callback(respond(drogon::k202Accepted));
}

} // namespace aid::controllers
