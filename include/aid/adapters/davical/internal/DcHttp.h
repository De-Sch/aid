#pragma once

// DcHttp — CardDAV REPORT request builder. See
// plan/classes/adapters/DcHttp.md and BACKEND_LOGIC.md §6.2.
//
// Wraps the daemon's shared HttpClient with a fixed Basic-auth header
// and the CardDAV-required Depth: 1 / Content-Type. Returns the raw
// multistatus XML body; parsing is the caller's job (DcVCardParser).
// No retry — failures bubble; the use case treats them as no-match.

#include <string>
#include <string_view>

#include "aid/infrastructure/HttpClient.h"
#include "aid/plumbing/Result.h"
#include "aid/plumbing/Task.h"

namespace aid::adapters::davical::internal {

class DcHttp {
public:
    // Borrows http; caller (DaviCalAdapter) owns it via unique_ptr and
    // outlives this. user + password become a precomputed
    // `Basic <base64>` Authorization header so we don't re-encode per
    // request.
    DcHttp(aid::infrastructure::HttpClient& http, std::string user, std::string password);

    DcHttp(const DcHttp&) = delete;
    DcHttp& operator=(const DcHttp&) = delete;
    DcHttp(DcHttp&&) = delete;
    DcHttp& operator=(DcHttp&&) = delete;
    ~DcHttp() = default;

    // url is taken by value (std::string): report/probe are coroutines that
    // read `url` when building the status Error after the co_await, so the
    // frame must own its storage (docs/issues/0004 §5). xmlBody is consumed
    // into the request before the suspension, so it stays borrowed.
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<std::string>>
    report(std::string url, std::string_view xmlBody);

    // Cheap auth'd PROPFIND (Depth: 0) used by DaviCalAdapter::ping(); a
    // CardDAV collection answers GET with 405, so we probe with PROPFIND.
    // 207 Multi-Status / any 2xx → ok; anything else is mapped to the same
    // ErrorCode taxonomy as report().
    [[nodiscard]] aid::plumbing::Task<aid::plumbing::Result<void>> probe(std::string url);

private:
    aid::infrastructure::HttpClient& http_;
    std::string authHeader_;
};

} // namespace aid::adapters::davical::internal
