#pragma once

#include <functional>
#include <nlohmann/json_fwd.hpp>
#include <string>
#include <string_view>

#include "aid/adapters/openproject/internal/HttpDispatcher.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

// OpHttp — Authenticated OpenProject request layer.
//
// Owns three things, and nothing else:
//
//   1. The "Basic apikey:<token>" header, built once at construction.
//   2. JSON-body get/post/patch helpers that return parsed nlohmann::json.
//   3. retryOn409 — the lockVersion retry loop
//      (50/100/200/400/800 ms backoff, max 5 attempts) used by every
//      PATCH callsite. OpTicketRepo::save delegates to retryOn409;
//      closeTwoStep walks through save once per step.
//
// Network-error retries (3 attempts at 50/100/200 ms) live inside
// HttpClient, not here — OpHttp only handles HTTP-409 Conflict.

namespace aid::adapters::openproject {

class OpHttp {
public:
    using PatchFn = std::function<aid::plumbing::Task<aid::plumbing::Result<nlohmann::json>>()>;
    using RefreshLockVersionFn = std::function<aid::plumbing::Task<aid::plumbing::Result<int>>()>;

    OpHttp(HttpDispatcher& dispatcher, std::string baseUrl, std::string_view apiToken,
           Sleeper sleeper);

    OpHttp(const OpHttp&) = delete;
    OpHttp& operator=(const OpHttp&) = delete;
    OpHttp(OpHttp&&) = delete;
    OpHttp& operator=(OpHttp&&) = delete;
    ~OpHttp() = default;

    // path is taken by value (std::string): these are coroutines that read
    // `path` when building the parse-error / status Error after the first
    // co_await, so the frame must own its storage.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<nlohmann::json>> get(std::string path);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<nlohmann::json>>
    post(std::string path, const nlohmann::json& body);

    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<nlohmann::json>>
    patch(std::string path, const nlohmann::json& body);

    // The retry loop. On HTTP-409 from patchFn: await
    // refreshLockVersion(), then sleep 50/100/200/400/800 ms (one per
    // attempt). Five conflicts in a row → LockVersionExhausted. Non-409
    // errors short-circuit and are returned to the caller verbatim.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<nlohmann::json>>
    retryOn409(PatchFn patchFn, RefreshLockVersionFn refreshLockVersion);

    [[nodiscard]] const std::string& baseUrl() const noexcept { return baseUrl_; }

private:
    HttpDispatcher& dispatcher_;
    std::string baseUrl_;
    std::string authHeader_; // "Basic " + base64("apikey:<token>")
    Sleeper sleeper_;
};

} // namespace aid::adapters::openproject
