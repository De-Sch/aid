#include "aid/infrastructure/HttpClient.h"

#include <drogon/HttpClient.h>
#include <drogon/HttpRequest.h>
#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>
#include <trantor/net/EventLoop.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

using namespace std::chrono_literals;

namespace aid::infrastructure {

namespace {

// Network-error backoff: 3 attempts at 50/100/200 ms. The 5-step
// {50,100,200,400,800} ms ladder is for the ticket system's 409 retries and lives
// in the adapter, not here.
constexpr std::array<std::chrono::milliseconds, 3> kBackoff{50ms, 100ms, 200ms};

struct SleepAwaiter {
    trantor::EventLoop& loop;
    std::chrono::milliseconds dur;

    [[nodiscard]] bool await_ready() const noexcept { return dur.count() <= 0; }

    void await_suspend(std::coroutine_handle<> h) const noexcept {
        const double secs = static_cast<double>(dur.count()) / 1000.0;
        loop.runAfter(secs, [h]() noexcept { h.resume(); });
    }

    void await_resume() const noexcept {}
};

[[nodiscard]] drogon::HttpMethod parseMethod(std::string_view method) noexcept {
    if (method == "GET") {
        return drogon::HttpMethod::Get;
    }
    if (method == "POST") {
        return drogon::HttpMethod::Post;
    }
    if (method == "PUT") {
        return drogon::HttpMethod::Put;
    }
    if (method == "DELETE") {
        return drogon::HttpMethod::Delete;
    }
    if (method == "PATCH") {
        return drogon::HttpMethod::Patch;
    }
    if (method == "HEAD") {
        return drogon::HttpMethod::Head;
    }
    if (method == "OPTIONS") {
        return drogon::HttpMethod::Options;
    }
    // WebDAV PROPFIND (RFC 4918 §9.1). Shipped natively in drogon::HttpMethod
    // (v1.9.13), unlike REPORT below. Used by the address-book reachability probe:
    // a CardDAV collection answers GET with 405 but PROPFIND with 207.
    if (method == "PROPFIND") {
        return drogon::HttpMethod::Propfind;
    }
    // CardDAV REPORT (RFC 3253). drogon::HttpMethod::Report was added to
    // v1.9.13 via cmake/patches/drogon-add-report-verb.patch; if that
    // patch isn't applied, the project won't link.
    if (method == "REPORT") {
        return drogon::HttpMethod::Report;
    }
    return drogon::HttpMethod::Invalid;
}

[[nodiscard]] HttpResponse mapResponse(const drogon::HttpResponsePtr& resp) {
    HttpResponse out;
    out.status = static_cast<int>(resp->getStatusCode());
    for (const auto& [k, v] : resp->getHeaders()) {
        out.headers.emplace(k, v);
    }
    const std::string_view body = resp->getBody();
    out.body.assign(body.data(), body.size());
    return out;
}

[[nodiscard]] bool equalsIgnoreCase(std::string_view a, std::string_view b) {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](unsigned char x, unsigned char y) {
               return std::tolower(x) == std::tolower(y);
           });
}

[[nodiscard]] drogon::HttpRequestPtr buildRequest(drogon::HttpMethod method, std::string_view path,
                                                  std::string_view body, const Headers& hdrs) {
    auto req = drogon::HttpRequest::newHttpRequest();
    req->setMethod(method);
    req->setPath(std::string{path});
    req->setPathEncode(false);
    if (!body.empty()) {
        req->setBody(std::string{body});
    }
    for (const auto& [k, v] : hdrs.kv) {
        // Drogon serializes Content-Type from the request's internal
        // content-type field, NOT from a generic header. A plain
        // addHeader("Content-Type", …) is dropped, leaving setBody()'s
        // text/plain default — which the ticket system rejects with HTTP 415.
        // Route Content-Type through the dedicated setter so the wire
        // header is honored (e.g. application/json for every POST/PATCH).
        if (equalsIgnoreCase(k, "Content-Type")) {
            req->setContentTypeString(v);
        } else {
            req->addHeader(k, v);
        }
    }
    return req;
}

} // namespace

namespace detail {

// Control block shared between a request's awaiter, drogon's completion
// callback, and the cancellation path. Heap-allocated and reference-counted so
// it outlives BOTH the coroutine frame AND the HttpClient: whichever path loses
// the `settled` race touches only this block, never freed memory. `settled` is
// the single source of truth for "the coroutine has been (or is being) resumed
// exactly once" — the loser is a strict no-op.
struct HttpReqControl {
    std::atomic<bool> settled{false};
    std::coroutine_handle<> handle{};
    // Outcome, written by the settle winner before it resumes the coroutine.
    drogon::ReqResult rc{drogon::ReqResult::Ok};
    drogon::HttpResponsePtr resp{};
    bool cancelled{false};
};

// Registry of in-flight requests for one HttpClient, plus a sticky cancelled
// flag. Lives behind a shared_ptr (see HttpClient::cancelStation_) so it can be
// captured by drogon completion callbacks that may fire after the owning
// HttpClient is destroyed.
class HttpCancelStation {
public:
    explicit HttpCancelStation(trantor::EventLoop& loop) : loop_(loop) {}

    void add(const std::shared_ptr<HttpReqControl>& c) {
        const std::scoped_lock lk{mtx_};
        inflight_.insert(c);
    }

    void remove(const std::shared_ptr<HttpReqControl>& c) {
        const std::scoped_lock lk{mtx_};
        inflight_.erase(c);
    }

    [[nodiscard]] bool cancelled() const noexcept {
        return cancelled_.load(std::memory_order_acquire);
    }

    // Resume every outstanding request with a cancelled outcome. Posts the
    // resume to the loop so the coroutine resumes on its owning loop,
    // never on the caller's thread. The `settled` exchange guards against a
    // concurrent drogon completion: the loser does nothing.
    std::size_t cancelAll() {
        cancelled_.store(true, std::memory_order_release);
        std::vector<std::shared_ptr<HttpReqControl>> snapshot;
        {
            const std::scoped_lock lk{mtx_};
            snapshot.assign(inflight_.begin(), inflight_.end());
        }
        for (const auto& c : snapshot) {
            loop_.queueInLoop([c] {
                if (c->settled.exchange(true)) {
                    return; // drogon's completion callback already resumed it
                }
                c->cancelled = true;
                c->handle.resume();
            });
        }
        return snapshot.size();
    }

private:
    trantor::EventLoop& loop_; // the domain loop; outlives this station
    mutable std::mutex mtx_;
    std::unordered_set<std::shared_ptr<HttpReqControl>> inflight_;
    std::atomic<bool> cancelled_{false};
};

namespace {

// Hand-rolled replacement for drogon's HttpRespAwaiter. Identical send path —
// it forwards to drogon::HttpClient::sendRequest with the same per-request
// timeout — but its completion callback resolves a heap control block rather
// than the coroutine frame directly. That indirection is what lets the
// shutdown path (HttpCancelStation::cancelAll) resume the coroutine early
// without racing drogon into a use-after-free of the frame.
class HttpRequestAwaiter {
public:
    HttpRequestAwaiter(std::shared_ptr<HttpCancelStation> station, drogon::HttpClientPtr client,
                       drogon::HttpRequestPtr req, double timeout)
        : station_(std::move(station)), client_(std::move(client)), req_(std::move(req)),
          timeout_(timeout), ctl_(std::make_shared<HttpReqControl>()) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> h) {
        ctl_->handle = h;
        station_->add(ctl_);
        // Capture only the shared control block and station — never the frame.
        // Both outlive the frame, so a callback that fires after a cancelled
        // resume (and after the frame is gone) is safe.
        client_->sendRequest(
            req_,
            [ctl = ctl_, station = station_](drogon::ReqResult rc,
                                             const drogon::HttpResponsePtr& resp) {
                station->remove(ctl);
                if (ctl->settled.exchange(true)) {
                    return; // cancelAll already resumed this coroutine
                }
                ctl->rc = rc;
                ctl->resp = resp;
                ctl->handle.resume();
            },
            timeout_);
    }

    [[nodiscard]] std::shared_ptr<HttpReqControl> await_resume() noexcept { return ctl_; }

private:
    std::shared_ptr<HttpCancelStation> station_;
    drogon::HttpClientPtr client_;
    drogon::HttpRequestPtr req_;
    double timeout_;
    std::shared_ptr<HttpReqControl> ctl_;
};

} // namespace

} // namespace detail

HttpClient::HttpClient(std::string_view baseUrl, UpstreamConfig cfg, trantor::EventLoop& loop)
    : baseUrl_(baseUrl), cfg_(cfg), loop_(loop),
      cancelStation_(std::make_shared<detail::HttpCancelStation>(loop)) {
}

HttpClient::~HttpClient() {
    // NOTE: ~HttpClient deliberately does NOT cancel in-flight requests. This
    // wrapper is owned by a plugin adapter and is destroyed LAST among the
    // adapter's members (it is declared first), so by the time we get here the
    // adapter's helpers (OpTicketRepo / OpHttp / …) are already gone — resuming
    // a suspended worker chain now would run it through freed plugin internals
    // (observed as a shutdown SIGSEGV). Cancellation is
    // instead driven from the shutdown sequence via the port's
    // cancelPendingRequests() hook WHILE the adapter is still fully alive, and
    // the daemon then waits for the mailboxes to go quiescent before releasing
    // the plugin. The cancelStation_ is held by shared_ptr, so any drogon
    // completion callback that fires after this dtor remains safe.
}

std::size_t HttpClient::cancelInFlight() noexcept {
    return cancelStation_->cancelAll();
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
HttpClient::send(std::string method, std::string path, std::string_view body, const Headers& hdrs) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;

    const drogon::HttpMethod m = parseMethod(method);
    if (m == drogon::HttpMethod::Invalid) {
        co_return aid::plumbing::unexpected{Error{
            ErrorCode::InvalidInput,
            "HttpClient::send: unsupported method '" + std::string{method} + "'", std::nullopt}};
    }

    const int attempts = std::max(1, cfg_.networkRetries);
    auto req = buildRequest(m, path, body, hdrs);
    const double timeoutSec = static_cast<double>(cfg_.readTimeout.count());

    // Fresh drogon client per send() call — never a persistent, shared one.
    // A reused client carries the previous request's connection: if the server
    // kept it alive and then closed it idle, the next request races that
    // teardown and can be written onto a dying socket (no response → timeout)
    // or buffered against drogon 1.9.13's stale tcpClientPtr_ (see HttpClient.h).
    // A per-call client starts with a null tcpClientPtr_ and connects cleanly;
    // it is destroyed once at co_return, so no client is torn down mid-loop
    // (which would crash inside its own loop callbacks). Intra-call retries
    // safely reuse this client: drogon's onError resets the connection on
    // NetworkFailure, so the next attempt reconnects.
    auto client = drogon::HttpClient::newHttpClient(baseUrl_, &loop_);

    // Capture everything reachable AFTER the first co_await into frame-owned
    // locals: a request that resumes once this HttpClient has been destroyed
    // (the shutdown-cancellation case) must not touch any HttpClient member.
    // The station is shared_ptr (frame-owned copy); the loop reference points
    // at the domain loop, which outlives this wrapper (HttpClient.h contract),
    // so a frame-owned pointer to it stays valid. `client` is a frame local
    // and is reused across intra-call retries (drogon's onError resets the
    // connection on NetworkFailure), so no member access is needed on retry.
    auto station = cancelStation_;
    trantor::EventLoop* const loop = &loop_;

    for (int attempt = 0; attempt < attempts; ++attempt) {
        // A request resumed by the backoff sleep below after cancellation was
        // requested must not issue a fresh upstream call on a dying wrapper.
        if (station->cancelled()) {
            co_return aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable,
                                                      "http " + method + " " + path + ": cancelled",
                                                      std::nullopt}};
        }

        auto ctl = co_await detail::HttpRequestAwaiter{station, client, req, timeoutSec};

        // Terminal cancellation: the shutdown path resumed us early. Do not
        // retry; surface immediately so the worker reaches final_suspend.
        if (ctl->cancelled) {
            co_return aid::plumbing::unexpected{Error{ErrorCode::UpstreamUnavailable,
                                                      "http " + method + " " + path + ": cancelled",
                                                      std::nullopt}};
        }
        if (ctl->rc == drogon::ReqResult::Ok) {
            co_return mapResponse(ctl->resp);
        }

        const drogon::ReqResult rc = ctl->rc;
        const bool retryable =
            rc == drogon::ReqResult::NetworkFailure || rc == drogon::ReqResult::Timeout;
        const bool lastAttempt = attempt + 1 >= attempts;

        if (retryable && !lastAttempt) {
            const auto idx = static_cast<std::size_t>(attempt);
            const auto sleepDur = idx < kBackoff.size() ? kBackoff[idx] : kBackoff.back();
            co_await SleepAwaiter{*loop, sleepDur};
        } else {
            // Boundary logging belongs to the adapter (where cid is in scope) /
            // mailbox worker. Propagate silently here.
            const ErrorCode ec = (rc == drogon::ReqResult::Timeout)
                                     ? ErrorCode::UpstreamTimeout
                                     : ErrorCode::UpstreamUnavailable;
            std::string msg = "http ";
            msg.append(method);
            msg.push_back(' ');
            msg.append(path);
            msg.append(": ");
            msg.append(drogon::to_string_view(rc));
            msg.append(" after ");
            msg.append(std::to_string(attempt + 1));
            msg.append(attempts == 1 ? " attempt" : " attempts");
            co_return aid::plumbing::unexpected{Error{ec, std::move(msg), std::nullopt}};
        }
    }

    co_return aid::plumbing::unexpected{
        Error{ErrorCode::Unknown, "HttpClient::send: retry loop exited without result (defect)",
              std::nullopt}};
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>> HttpClient::get(std::string_view path,
                                                                         const Headers& hdrs) {
    return send("GET", std::string{path}, {}, hdrs);
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
HttpClient::post(std::string_view path, std::string_view body, const Headers& hdrs) {
    return send("POST", std::string{path}, body, hdrs);
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
HttpClient::patch(std::string_view path, std::string_view body, const Headers& hdrs) {
    return send("PATCH", std::string{path}, body, hdrs);
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>> HttpClient::del(std::string_view path,
                                                                         const Headers& hdrs) {
    return send("DELETE", std::string{path}, {}, hdrs);
}

aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
HttpClient::report(std::string_view path, std::string_view body, const Headers& hdrs) {
    return send("REPORT", std::string{path}, body, hdrs);
}

} // namespace aid::infrastructure
