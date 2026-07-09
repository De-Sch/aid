#include "aid/adapters/davical/internal/DcHttp.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

#include "aid/adapters/support/HttpSupport.h"
#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Error.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace aid::adapters::davical::internal {

namespace {

// HTTP status → ErrorCode for the REPORT response. Mirrors
// lib/adapters/openproject_plugin/internal/OpHttp.cpp::httpStatusError
// so the two plugins map status codes consistently.
[[nodiscard]] aid::plumbing::Error reportStatusError(int status, std::string_view url,
                                                     std::string_view body) {
    using aid::plumbing::Error;
    using aid::plumbing::ErrorCode;
    ErrorCode ec = ErrorCode::UpstreamUnavailable;
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
    default:
        ec = ErrorCode::UpstreamUnavailable;
        break;
    }
    std::string msg;
    msg.reserve(body.size() + 64);
    msg.append("REPORT ");
    msg.append(url);
    msg.append(" -> ");
    msg.append(std::to_string(status));
    if (!body.empty()) {
        msg.append(" body=");
        // error-handling.md: cap body in error message at 200 chars
        // (truncating untrusted CardDAV output we surface upstream).
        constexpr std::size_t kCap = 200;
        if (body.size() > kCap) {
            msg.append(body.substr(0, kCap));
            // ASCII "..." rather than U+2026 so a downstream log sink
            // that assumes ASCII-only Error::message doesn't see
            // \xE2\x80\xA6.
            msg.append("...");
        } else {
            msg.append(body);
        }
    }
    return Error{ec, std::move(msg), std::nullopt};
}

} // namespace

DcHttp::DcHttp(aid::infrastructure::HttpClient& http, std::string user, std::string password)
    : http_(http),
      // operator+ evaluation order is unspecified but both args are
      // independent locals (no aliasing), so both end up moved-from
      // regardless of which side reads first.
      authHeader_("Basic " + aid::adapters::support::base64Encode(std::move(user) + ":" +
                                                                  std::move(password))) {
    // RFC 7617 — Basic auth header is "Basic " + base64(user:password).
    // Built once at construction so we don't re-encode per request; the
    // password lives in this string until DcHttp's destructor (which is
    // tied to DaviCalAdapter's lifetime). Never logged (Config.md
    // secrets-at-rest).
}

aid::plumbing::Task<aid::plumbing::Result<std::string>> DcHttp::report(std::string url,
                                                                       std::string_view xmlBody) {
    aid::infrastructure::Headers h;
    h.kv.emplace_back("Depth", "1");
    h.kv.emplace_back("Content-Type", "application/xml; charset=utf-8");
    h.kv.emplace_back("Authorization", authHeader_);

    auto resp = co_await http_.report(url, xmlBody, h);
    if (!resp) {
        co_return aid::plumbing::unexpected{resp.error()};
    }
    if (resp->status < 200 || resp->status >= 300) {
        co_return aid::plumbing::unexpected{reportStatusError(resp->status, url, resp->body)};
    }
    co_return std::move(resp->body);
}

aid::plumbing::Task<aid::plumbing::Result<void>> DcHttp::probe(std::string url) {
    // A CardDAV *collection* answers GET with 405 (Method Not Allowed); it
    // serves PROPFIND/REPORT. Probe with a cheap PROPFIND Depth: 0 (no body)
    // so a healthy DaviCal — which replies 207 Multi-Status — reads as
    // reachable. Same auth path as the real lookup() flow.
    aid::infrastructure::Headers h;
    h.kv.emplace_back("Depth", "0");
    h.kv.emplace_back("Authorization", authHeader_);

    auto resp = co_await http_.send("PROPFIND", url, {}, h);
    if (!resp) {
        co_return aid::plumbing::unexpected{resp.error()};
    }
    if (resp->status < 200 || resp->status >= 300) {
        co_return aid::plumbing::unexpected{reportStatusError(resp->status, url, resp->body)};
    }
    co_return aid::plumbing::Result<void>{};
}

} // namespace aid::adapters::davical::internal
