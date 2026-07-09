#include "aid/adapters/openproject/internal/OpHttp.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <nlohmann/json.hpp>
#include <string>
#include <utility>

#include "aid/adapters/support/HttpSupport.h"
#include "aid/plumbing/Error.h"

using aid::plumbing::Error;
using aid::plumbing::ErrorCode;
using aid::plumbing::Result;
using aid::plumbing::Task;
using aid::plumbing::unexpected;
using namespace std::chrono_literals;

namespace aid::adapters::openproject {

namespace {

// lockVersion 409 retry table. Five attempts total, sleeps
// between consecutive PATCHes. The total worst-case sleep is
// 50+100+200+400+800 = 1550 ms. Defined here (not in OpHttp.h) so
// callers can't depend on the exact ladder by reaching for the constant.
constexpr int kMaxConflictAttempts = 5;
constexpr std::array<std::chrono::milliseconds, kMaxConflictAttempts> kConflictBackoff{
    50ms, 100ms, 200ms, 400ms, 800ms};

aid::infrastructure::Headers jsonHeaders(const std::string& authHeader, bool withContentType) {
    aid::infrastructure::Headers h;
    h.kv.emplace_back("Authorization", authHeader);
    h.kv.emplace_back("Accept", "application/json");
    if (withContentType) {
        h.kv.emplace_back("Content-Type", "application/json");
    }
    return h;
}

Error httpStatusError(int status, std::string_view method, std::string_view path,
                      std::string_view body) {
    ErrorCode ec = ErrorCode::Unknown;
    switch (status) {
    case 401:
        ec = ErrorCode::Unauthenticated;
        break;
    case 403:
        ec = ErrorCode::Forbidden;
        break;
    case 404:
        ec = ErrorCode::NotFound;
        break;
    case 409:
        ec = ErrorCode::Conflict409;
        break;
    case 422:
        ec = ErrorCode::Unprocessable422;
        break;
    default:
        ec = ErrorCode::UpstreamUnavailable;
        break;
    }
    std::string msg;
    msg.reserve(body.size() + 64);
    msg.append(method);
    msg.push_back(' ');
    msg.append(path);
    msg.append(" -> ");
    msg.append(std::to_string(status));
    if (!body.empty()) {
        msg.append(" body=");
        // Cap body in error message at 200 chars.
        constexpr std::size_t kCap = 200;
        if (body.size() > kCap) {
            msg.append(body.substr(0, kCap));
            msg.append("…");
        } else {
            msg.append(body);
        }
    }
    return Error{ec, std::move(msg), std::nullopt};
}

Result<nlohmann::json> parseJsonBody(const aid::infrastructure::HttpResponse& resp,
                                     std::string_view method, std::string_view path) {
    if (resp.status < 200 || resp.status >= 300) {
        return unexpected(httpStatusError(resp.status, method, path, resp.body));
    }
    if (resp.body.empty()) {
        return nlohmann::json::object();
    }
    try {
        return nlohmann::json::parse(resp.body);
    } catch (const nlohmann::json::parse_error& e) {
        std::string msg;
        msg.append(method);
        msg.push_back(' ');
        msg.append(path);
        msg.append(": response is not valid JSON: ");
        msg.append(e.what());
        return unexpected(Error{ErrorCode::Unknown, std::move(msg), std::nullopt});
    }
}

} // namespace

OpHttp::OpHttp(HttpDispatcher& dispatcher, std::string baseUrl, std::string_view apiToken,
               Sleeper sleeper)
    : dispatcher_(dispatcher), baseUrl_(std::move(baseUrl)),
      authHeader_("Basic " +
                  aid::adapters::support::base64Encode("apikey:" + std::string{apiToken})),
      sleeper_(std::move(sleeper)) {
}

Task<Result<nlohmann::json>> OpHttp::get(std::string path) {
    const auto headers = jsonHeaders(authHeader_, /*withContentType=*/false);
    auto resp = co_await dispatcher_.send("GET", path, {}, headers);
    if (!resp)
        co_return unexpected(resp.error());
    co_return parseJsonBody(*resp, "GET", path);
}

Task<Result<nlohmann::json>> OpHttp::post(std::string path, const nlohmann::json& body) {
    const auto headers = jsonHeaders(authHeader_, /*withContentType=*/true);
    const std::string serialized = body.dump();
    auto resp = co_await dispatcher_.send("POST", path, serialized, headers);
    if (!resp)
        co_return unexpected(resp.error());
    co_return parseJsonBody(*resp, "POST", path);
}

Task<Result<nlohmann::json>> OpHttp::patch(std::string path, const nlohmann::json& body) {
    const auto headers = jsonHeaders(authHeader_, /*withContentType=*/true);
    const std::string serialized = body.dump();
    auto resp = co_await dispatcher_.send("PATCH", path, serialized, headers);
    if (!resp)
        co_return unexpected(resp.error());
    co_return parseJsonBody(*resp, "PATCH", path);
}

Task<Result<nlohmann::json>> OpHttp::retryOn409(PatchFn patchFn,
                                                RefreshLockVersionFn refreshLockVersion) {
    // Algorithm:
    //
    //   for attempt in 0..5:
    //     r = await patchFn()
    //     if r is OK:                          return r
    //     if r.error.code != Conflict409:      return r
    //     await sleep(kConflictBackoff[attempt])
    //     await refreshLockVersion()
    //   return Error{LockVersionExhausted}
    //
    // Sleep happens BEFORE refresh (matches the "sleep ... refresh"
    // order). Refresh callback is expected to mutate the ticket the
    // caller closes over so the next patchFn() invocation sees the
    // freshly fetched lockVersion.
    for (int attempt = 0; attempt < kMaxConflictAttempts; ++attempt) {
        auto r = co_await patchFn();
        if (r) {
            co_return *r;
        }
        if (r.error().code != ErrorCode::Conflict409) {
            co_return unexpected(r.error());
        }

        co_await sleeper_(kConflictBackoff[static_cast<std::size_t>(attempt)]);

        auto refreshed = co_await refreshLockVersion();
        if (!refreshed) {
            co_return unexpected(refreshed.error());
        }
    }
    co_return unexpected(Error{ErrorCode::LockVersionExhausted,
                               "lockVersion exhausted after 5 retries", std::nullopt});
}

} // namespace aid::adapters::openproject
