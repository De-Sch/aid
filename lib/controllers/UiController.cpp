#include "aid/controllers/UiController.h"

#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoop.h>

#include <cctype>
#include <exception>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "aid/controllers/ControllerSupport.h"
#include "aid/controllers/SessionGuard.h"
#include "aid/crosscutting/CorrelationId.h"
#include "aid/crosscutting/Logger.h"
#include "aid/plumbing/ActionResult.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"
#include "aid/serialization/ActionResultJson.h"
#include "aid/serialization/DashboardJson.h"
#include "aid/usecases/AppendComment.h"
#include "aid/usecases/CloseTicket.h"
#include "aid/usecases/GetDashboard.h"
#include "aid/value-types/Dashboard.h"
#include "aid/value-types/Ids.h"

namespace aid::controllers {

using aid::crosscutting::LogType;

namespace {

// Invoke the response callback `cb` on the connection's own loop.
// On the success path the coroutine has already hopped back via
// `co_await resumeOn(connLoop)`, so this is an inline call there. The catch
// handlers, however, may run on the domain loop (an exception thrown out of
// the upstream `co_await`) AND C++ forbids `co_await` inside a catch handler —
// so the cross-loop hop on the error path is done by posting `cb` directly.
// `loop == nullptr` (a unit test with no running loop) or already-in-loop both
// resolve to a plain inline call.
void respondOnLoop(trantor::EventLoop* loop,
                   const std::function<void(const drogon::HttpResponsePtr&)>& cb,
                   const drogon::HttpResponsePtr& resp) {
    if (loop != nullptr && !loop->isInLoopThread()) {
        loop->queueInLoop([cb, resp]() { cb(resp); });
    } else {
        cb(resp);
    }
}

[[nodiscard]] bool isValidTicketId(std::string_view s) noexcept {
    if (s.empty() || s.size() > 12) {
        return false;
    }
    for (char c : s) {
        if (c < '0' || c > '9') {
            return false;
        }
    }
    return true;
}

[[nodiscard]] const char* toString(aid::AddressKind k) noexcept {
    switch (k) {
    case aid::AddressKind::Person:
        return "Person";
    case aid::AddressKind::Company:
        return "Company";
    }
    return "Person";
}

[[nodiscard]] nlohmann::json toJson(const aid::Contact& c) {
    nlohmann::json j;
    j["name"] = c.name;
    j["companyName"] = c.companyName;
    j["kind"] = toString(c.kind);
    auto phones = nlohmann::json::array();
    for (const auto& p : c.phoneNumbers) {
        phones.push_back(p.v);
    }
    j["phoneNumbers"] = std::move(phones);
    auto projects = nlohmann::json::array();
    for (const auto& p : c.projectIds) {
        projects.push_back(p.v);
    }
    j["projectIds"] = std::move(projects);
    return j;
}

[[nodiscard]] nlohmann::json toJson(const aid::ActiveCall& a) {
    nlohmann::json j;
    j["ticketId"] = a.ticketId.v;
    j["callId"] = a.callId.v;
    j["projectName"] = a.projectName;
    j["callerNumber"] = a.callerNumber.v;
    return j;
}

[[nodiscard]] nlohmann::json toJson(const aid::DashboardView& v) {
    nlohmann::json j;
    auto tickets = nlohmann::json::array();
    for (const auto& e : v.tickets) {
        tickets.push_back(aid::serialization::toJson(e));
    }
    j["tickets"] = std::move(tickets);
    if (v.active.has_value()) {
        j["active"] = toJson(*v.active);
    } else {
        j["active"] = nullptr;
    }
    if (v.addressCallInformation.has_value()) {
        j["addressCallInformation"] = toJson(*v.addressCallInformation);
    } else {
        j["addressCallInformation"] = nullptr;
    }
    return j;
}

[[nodiscard]] nlohmann::json toJson(const aid::plumbing::ActionResult& ar) {
    // Delegate to the shared projection so the REST body and the WS
    // action_result frame never drift. finishOk resolves this overload via
    // ordinary lookup for its ActionResult results.
    return aid::serialization::toJson(ar);
}

[[nodiscard]] std::optional<std::string> commentText(const std::string& body) {
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(body);
    } catch (const nlohmann::json::parse_error&) {
        return std::nullopt;
    }
    auto it = j.find("comment");
    if (it == j.end() || !it->is_string()) {
        return std::nullopt;
    }
    return it->get<std::string>();
}

} // namespace

UiController::UiController(aid::usecases::GetDashboard& dashboard,
                           aid::usecases::AppendComment& comment, aid::usecases::CloseTicket& close,
                           aid::crosscutting::CorrelationId& cid,
                           aid::crosscutting::Logger& logger) noexcept
    : dashboard_(dashboard), comment_(comment), close_(close), cid_(cid), logger_(logger) {
}

// Shared tail of every /ui action, run AFTER the body has hopped back onto the
// connection loop via resumeOn(connLoop). On use-case failure it logs and
// returns the uniform 500; on success it logs the per-action `detail` and
// serialises the result. `toJson` overloads on the concrete result type
// (DashboardView / ActionResult), so the success body is byte-identical to the
// old per-method code. `detail` is a callable `(const T&) -> std::string`,
// invoked only on the success path (so it may dereference the result).
template <typename R, typename Detail>
drogon::HttpResponsePtr UiController::finishOk(const R& r, std::string_view op,
                                               const std::string& cidStr, Detail detail) {
    if (!r.has_value()) {
        logger_.error(std::string{"UiController."} + std::string{op} +
                          ": usecase failed: " + r.error().message,
                      LogType::FRONTEND, cidStr);
        // Map the domain code to the right HTTP status (was collapsed to 500,
        // which hid 404/409/422/429/502/504 the domain already carried). Body
        // stays the generic {"error":"internal"} the SPA already handles.
        return jsonResponse(httpStatusForError(r.error().code), R"({"error":"internal"})");
    }
    logger_.debug(std::string{"UiController."} + std::string{op} + ": " + detail(*r),
                  LogType::FRONTEND, cidStr);
    return jsonResponse(drogon::k200OK, toJson(*r).dump());
}

// Shared skeleton for the three /ui actions. Captures the connection's loop
// BEFORE the first suspension: Drogon runs this on the listener IO loop that
// owns the connection; the upstream co_await inside `body` resumes on the
// dedicated domain loop, and `body` hops back via resumeOn(connLoop) before
// returning, so the single `respond` here always writes on the connection's
// loop. `body` is a per-action coroutine that
// performs validation, the use-case co_await, the hop back, and produces the
// HTTP response; any exception escaping it becomes the uniform 500. The
// idempotent `respond` sink fires at most once, so a throw from a success-path
// serialisation can't make the catch handler send a second response.
template <typename Body>
drogon::AsyncTask UiController::dispatch(drogon::HttpRequestPtr req, Callback cb,
                                         std::string_view op, Body body) {
    trantor::EventLoop* const connLoop = trantor::EventLoop::getEventLoopOfCurrentThread();
    const std::string cidStr = cid_.nextUuid();
    bool responded = false;
    auto respond = [&](const drogon::HttpResponsePtr& resp) {
        if (responded) {
            return;
        }
        responded = true;
        respondOnLoop(connLoop, cb, resp);
    };
    try {
        respond(co_await body(req, connLoop, cidStr));
    } catch (const std::exception& e) {
        logger_.error(std::string{"UiController."} + std::string{op} + ": exception: " + e.what(),
                      LogType::FRONTEND, cidStr);
        respond(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
    } catch (...) {
        logger_.error(std::string{"UiController."} + std::string{op} + ": unknown exception",
                      LogType::FRONTEND, cidStr);
        respond(jsonResponse(drogon::k500InternalServerError, R"({"error":"internal"})"));
    }
}

void UiController::getDashboard(const drogon::HttpRequestPtr& req, Callback&& cb) {
    dispatch(req, std::move(cb), "dashboard",
             [this](const drogon::HttpRequestPtr& reqPtr, trantor::EventLoop* connLoop,
                    const std::string& cidStr) -> aid::plumbing::Task<drogon::HttpResponsePtr> {
                 const auto& viewer =
                     reqPtr->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
                 if (viewer.v.empty()) {
                     logger_.warn("UiController.dashboard: missing viewer attribute (filter bug?)",
                                  LogType::FRONTEND, cidStr);
                     co_return jsonResponse(drogon::k500InternalServerError,
                                            R"({"error":"unauthenticated"})");
                 }
                 auto r = co_await dashboard_.run(viewer);
                 // Back onto the connection's loop before producing the response.
                 co_await aid::plumbing::resumeOn(connLoop);
                 co_return finishOk(r, "dashboard", cidStr, [&](const aid::DashboardView& v) {
                     return "served viewer=" + viewer.v +
                            " tickets=" + std::to_string(v.tickets.size());
                 });
             });
}

void UiController::postComment(const drogon::HttpRequestPtr& req, Callback&& cb,
                               std::string ticketId) {
    dispatch(req, std::move(cb), "comment",
             [this, ticketId = std::move(ticketId)](
                 const drogon::HttpRequestPtr& reqPtr, trantor::EventLoop* connLoop,
                 const std::string& cidStr) -> aid::plumbing::Task<drogon::HttpResponsePtr> {
                 if (!isValidTicketId(ticketId)) {
                     co_return jsonResponse(drogon::k404NotFound, R"({"error":"not found"})");
                 }
                 const auto& viewer =
                     reqPtr->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
                 if (viewer.v.empty()) {
                     logger_.warn("UiController.comment: missing viewer attribute (filter bug?)",
                                  LogType::FRONTEND, cidStr);
                     co_return jsonResponse(drogon::k500InternalServerError,
                                            R"({"error":"unauthenticated"})");
                 }
                 auto textOpt = commentText(std::string{reqPtr->getBody()});
                 if (!textOpt.has_value()) {
                     co_return jsonResponse(drogon::k400BadRequest, R"({"error":"bad request"})");
                 }
                 // Hoist the TicketId into a named local so its lifetime is bound
                 // to the coroutine frame, not an awaited-temporary slot. GCC 12
                 // coroutines mis-destroy aggregate temporaries in co_await
                 // arguments (the temporary's std::string can be freed twice).
                 aid::TicketId tid;
                 tid.v = ticketId;
                 auto r = co_await comment_.run(tid, *textOpt, viewer);
                 // Back onto the connection's loop before producing the response.
                 co_await aid::plumbing::resumeOn(connLoop);
                 co_return finishOk(r, "comment", cidStr, [&](const aid::plumbing::ActionResult&) {
                     return "ticket=" + tid.v + " viewer=" + viewer.v;
                 });
             });
}

void UiController::postClose(const drogon::HttpRequestPtr& req, Callback&& cb,
                             std::string ticketId) {
    dispatch(req, std::move(cb), "close",
             [this, ticketId = std::move(ticketId)](
                 const drogon::HttpRequestPtr& reqPtr, trantor::EventLoop* connLoop,
                 const std::string& cidStr) -> aid::plumbing::Task<drogon::HttpResponsePtr> {
                 if (!isValidTicketId(ticketId)) {
                     co_return jsonResponse(drogon::k404NotFound, R"({"error":"not found"})");
                 }
                 const auto& viewer =
                     reqPtr->attributes()->get<aid::UserHandle>(SessionGuard::VIEWER_KEY);
                 if (viewer.v.empty()) {
                     logger_.warn("UiController.close: missing viewer attribute (filter bug?)",
                                  LogType::FRONTEND, cidStr);
                     co_return jsonResponse(drogon::k500InternalServerError,
                                            R"({"error":"unauthenticated"})");
                 }
                 aid::TicketId tid;
                 tid.v = ticketId;
                 auto r = co_await close_.run(tid, viewer);
                 // Back onto the connection's loop before producing the response.
                 co_await aid::plumbing::resumeOn(connLoop);
                 co_return finishOk(r, "close", cidStr, [&](const aid::plumbing::ActionResult&) {
                     return "ticket=" + tid.v + " viewer=" + viewer.v;
                 });
             });
}

} // namespace aid::controllers
