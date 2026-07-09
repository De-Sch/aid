#pragma once

#include <chrono>
#include <functional>
#include <string>
#include <string_view>

#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

// HttpDispatcher — the thin seam between OpHttp and aid::infrastructure::
// HttpClient. The concrete HttpClient is non-virtual (its v-table would be
// unstable across the .so boundary), so this abstract class exists solely
// to give tests a fake to inject in place of a real Drogon-backed client.
//
// Production wires RealHttpDispatcher (wraps an HttpClient&); tests build
// a recording subclass that captures the (method, path, body, headers)
// tuple and replies with a scripted HttpResponse.
//
// Sleeper is an injected awaitable-returning function so OpHttp's
// 50/100/200/400/800 ms backoff can be unit-tested without burning real
// wall time. Production passes a function that schedules on the domain
// EventLoop; tests pass an instant-resume that records the requested
// duration into a vector for assertion.

namespace aid::adapters::openproject {

class HttpDispatcher {
public:
    virtual ~HttpDispatcher() = default;

    HttpDispatcher() = default;
    HttpDispatcher(const HttpDispatcher&) = delete;
    HttpDispatcher& operator=(const HttpDispatcher&) = delete;
    HttpDispatcher(HttpDispatcher&&) = delete;
    HttpDispatcher& operator=(HttpDispatcher&&) = delete;

    [[nodiscard]] virtual aid::plumbing::Task<
        aid::plumbing::Result<aid::infrastructure::HttpResponse>>
    send(std::string_view method, std::string_view path, std::string_view body,
         const aid::infrastructure::Headers& hdrs) = 0;
};

using Sleeper = std::function<aid::plumbing::Task<void>(std::chrono::milliseconds)>;

// Production adapter: forwards every send() to a real HttpClient.
class RealHttpDispatcher final : public HttpDispatcher {
public:
    explicit RealHttpDispatcher(aid::infrastructure::HttpClient& http) noexcept : http_(http) {}

    // Trivial forwarder (not a coroutine — hands back HttpClient's Task);
    // kept inline so RealHttpDispatcher needs no translation unit.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<aid::infrastructure::HttpResponse>>
    send(std::string_view method, std::string_view path, std::string_view body,
         const aid::infrastructure::Headers& hdrs) override {
        return http_.send(std::string{method}, std::string{path}, body, hdrs);
    }

private:
    aid::infrastructure::HttpClient& http_;
};

} // namespace aid::adapters::openproject
