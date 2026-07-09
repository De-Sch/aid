#pragma once

#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/utils/coroutine.h>

#include <functional>
#include <string>
#include <string_view>

namespace aid::usecases {
class GetDashboard;
class AppendComment;
class CloseTicket;
} // namespace aid::usecases

namespace aid::crosscutting {
class CorrelationId;
class Logger;
} // namespace aid::crosscutting

namespace aid::controllers {

// Drogon HTTP handler for the three /ui/* business routes:
//   GET      /ui/dashboard
//   POST     /ui/comment/{ticketId}
//   POST     /ui/close/{ticketId}
//
// Owns JSON I/O and HTTP-status mapping; defers all business logic to
// GetDashboard / AppendComment / CloseTicket. Reads the authenticated
// viewer from request attributes (SessionGuard::VIEWER_KEY) — never
// from query or path.
//
// The public methods keep the framework's classic `void(req, cb&&)`
// signature; each one delegates to the private `dispatch` coroutine
// (a drogon::AsyncTask) with a per-action `body` lambda. `dispatch`
// owns the shared skeleton — capturing the connection loop, the
// idempotent single-response sink, and the try/catch that emits the
// uniform 500 JSON — while each `body` supplies only the validation,
// the use-case co_await, and the response. AsyncTask runs eagerly
// until first real suspension and is fire-and-forget (drogon
// LOG_FATALs on uncaught exceptions, hence the wrapping try/catch).
//
// Route registration (path-param regex `^[0-9]{1,12}$` on ticketId)
// lives in Main alongside SessionGuard installation; this class
// additionally re-checks ticketId shape so a misregistered route
// can't reach the ports with caller-controlled junk.
class UiController {
public:
    using Callback = std::function<void(const drogon::HttpResponsePtr&)>;

    UiController(aid::usecases::GetDashboard& dashboard, aid::usecases::AppendComment& comment,
                 aid::usecases::CloseTicket& close, aid::crosscutting::CorrelationId& cid,
                 aid::crosscutting::Logger& logger) noexcept;

    UiController(const UiController&) = delete;
    UiController& operator=(const UiController&) = delete;
    UiController(UiController&&) = delete;
    UiController& operator=(UiController&&) = delete;
    ~UiController() = default;

    void getDashboard(const drogon::HttpRequestPtr& req, Callback&& cb);
    void postComment(const drogon::HttpRequestPtr& req, Callback&& cb, std::string ticketId);
    void postClose(const drogon::HttpRequestPtr& req, Callback&& cb, std::string ticketId);

private:
    // Shared skeleton for the three /ui actions; `body` is a per-action
    // coroutine `(req, connLoop, cidStr) -> Task<HttpResponsePtr>`. Defined in
    // the .cpp — instantiated only there, so it stays out of this header.
    template <typename Body>
    drogon::AsyncTask dispatch(drogon::HttpRequestPtr req, Callback cb, std::string_view op,
                               Body body);

    // Shared success/failure tail (run after the body hops back to connLoop):
    // `R` is the use-case Result<T>, `detail` is `(const T&) -> std::string`
    // for the per-action debug line. Also .cpp-local.
    template <typename R, typename Detail>
    drogon::HttpResponsePtr finishOk(const R& r, std::string_view op, const std::string& cidStr,
                                     Detail detail);

    aid::usecases::GetDashboard& dashboard_;
    aid::usecases::AppendComment& comment_;
    aid::usecases::CloseTicket& close_;
    aid::crosscutting::CorrelationId& cid_;
    aid::crosscutting::Logger& logger_;
};

} // namespace aid::controllers
