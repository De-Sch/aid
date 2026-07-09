#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace drogon {
class HttpClient;
}

namespace trantor {
class EventLoop;
}

namespace aid::infrastructure::detail {
// Tracks every send() request currently suspended in an upstream co_await so
// the shutdown path can resume them at once. Defined in HttpClient.cpp; held by
// shared_ptr so a drogon completion callback firing AFTER this HttpClient is
// gone still finds a live control block. See cancelInFlight() below.
class HttpCancelStation;
} // namespace aid::infrastructure::detail

// HttpClient. Wraps drogon::HttpClient to expose a
// Task<Result<HttpResponse>> surface plus the network-error retry
// policy (3 attempts, 50/100/200 ms backoff). One instance per upstream
// (the ticket system, the address system), pinned to the single domain loop so all callback
// resumes land on the loop the caller awaits on.
//
// CONNECTION LIFETIME: each send() call builds a FRESH drogon::HttpClient,
// so no request ever inherits a connection a *prior* request left behind.
// This sidesteps a drogon 1.9.13 keep-alive hazard: after a request whose
// connection the server kept alive then closed when idle, a reused client
// races that teardown — the next request is written onto a dying socket (no
// response → timeout) or buffered against a stale tcpClientPtr_ without a
// reconnect and never sent — wedging follow-on writes (the GET-then-PATCH
// save path, the GET-then-POST create path). A per-call client starts with a
// null tcpClientPtr_ and connects cleanly. Intra-call retries reuse this one
// client safely (drogon's onError resets the connection on NetworkFailure),
// and it is destroyed exactly once at co_return — never mid-loop, which would
// crash inside its own loop callbacks. The drogon per-request timeout
// (UpstreamConfig readTimeout, > 0) is the bounded deadline: it arms before
// the connect step, so a stuck attempt resolves to UpstreamTimeout, not a hang.
//
// SHUTDOWN CANCELLATION: every in-flight send() is
// registered in a shared cancellation station. cancelInFlight() resumes each
// outstanding request at once with a terminal "cancelled" Error instead of
// letting it wait out readTimeout. It is driven from the shutdown sequence via
// the owning port's cancelPendingRequests() hook, which the daemon calls DURING
// the graceful drain — while the plugin (and every helper a suspended worker
// chain references) is still fully alive — and then waits for the mailboxes to
// go quiescent before releasing the plugin. That collapses the window in which
// a mailbox worker is still suspended in an upstream co_await when ~Mailbox
// destroys its frame. ~HttpClient does NOT cancel: this wrapper is destroyed
// last among the adapter's members, so resuming a chain here would run it
// through already-freed plugin helpers. The station outlives the HttpClient
// (shared_ptr), so a drogon completion callback that fires later — its
// per-request timeout had not yet elapsed — touches only the heap control
// block, never the freed wrapper.
//
// v1 is HTTP-only; TLS is deferred. 409 / lockVersion
// retries are NOT here — they live in the ticket-system adapter, since
// they need an adapter-specific refresh callback.

namespace aid::infrastructure {

struct HttpResponse {
    int status{0};
    std::unordered_map<std::string, std::string> headers;
    std::string body;
};

struct Headers {
    std::vector<std::pair<std::string, std::string>> kv;
};

// Mirrors crosscutting::Config::Upstream by shape. Defined here rather than
// pulled from Config.h so this header doesn't carry the JSON edge into
// infrastructure consumers (and so tests can construct one without a real
// Config). Main converts Config::Upstream → UpstreamConfig at the call site.
struct UpstreamConfig {
    std::chrono::seconds connectTimeout{5};
    std::chrono::seconds readTimeout{30};
    int networkRetries{3};
};

class HttpClient {
public:
    // baseUrl form: "http://host:port" (no trailing path). loop must outlive
    // the HttpClient. Each request's drogon::HttpClient is created on `loop`;
    // every awaiter resumes on `loop`, so callers must co_await from that loop.
    HttpClient(std::string_view baseUrl, UpstreamConfig cfg, trantor::EventLoop& loop);

    HttpClient(const HttpClient&) = delete;
    HttpClient& operator=(const HttpClient&) = delete;
    HttpClient(HttpClient&&) = delete;
    HttpClient& operator=(HttpClient&&) = delete;
    ~HttpClient();

    // method/path are taken by value (std::string): send() is a coroutine
    // that reads them on the error path after the first co_await, so the
    // frame must own their storage. body/hdrs are
    // consumed into the request before the first suspension, so they stay
    // borrowed.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    send(std::string method, std::string path, std::string_view body, const Headers& hdrs);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    get(std::string_view path, const Headers& hdrs);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    post(std::string_view path, std::string_view body, const Headers& hdrs);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    patch(std::string_view path, std::string_view body, const Headers& hdrs);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    del(std::string_view path, const Headers& hdrs);

    // CardDAV REPORT (RFC 3253). Drogon v1.9.13 doesn't ship the verb in
    // HttpMethod; cmake/patches/drogon-add-report-verb.patch adds it.
    // The address-book adapter's queries use REPORT exclusively.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<HttpResponse>>
    report(std::string_view path, std::string_view body, const Headers& hdrs);

    // Resume every request currently suspended in an upstream co_await at once,
    // each with a terminal Error{UpstreamUnavailable, "...: cancelled"}, instead
    // of letting it wait out readTimeout. Driven from the shutdown sequence via
    // the owning port's cancelPendingRequests() hook (TicketStore/AddressBook),
    // so a worker stuck on a hung upstream reaches final_suspend promptly — see
    // the SHUTDOWN CANCELLATION note above. Idempotent
    // and thread-safe: resumes are posted to the domain loop; the wrapper need
    // not outlive them. Terminal — it also flips a sticky cancelled flag so any
    // later send() short-circuits. Returns how many requests it signalled.
    std::size_t cancelInFlight() noexcept;

private:
    // Stored rather than a persistent drogon client: send() builds a fresh
    // drogon::HttpClient per attempt from this base URL (see class comment).
    std::string baseUrl_;
    UpstreamConfig cfg_;
    trantor::EventLoop& loop_;
    // In-flight request registry for shutdown cancellation. shared_ptr so it
    // outlives this wrapper for any late-firing drogon completion callback.
    std::shared_ptr<detail::HttpCancelStation> cancelStation_;
};

} // namespace aid::infrastructure
