#pragma once

#include <drogon/HttpResponse.h>
#include <drogon/HttpTypes.h>

#include <string>
#include <string_view>

#include "aid/plumbing/Error.h"

namespace aid::controllers {

// Build a JSON HTTP response with the given status code and pre-serialized
// body. Shared by UiController and LoginController, which previously each
// carried a byte-identical copy in their anonymous namespaces. Kept inline
// (header-only) so no new translation unit / link edge is introduced.
[[nodiscard]] inline drogon::HttpResponsePtr jsonResponse(drogon::HttpStatusCode code,
                                                          std::string_view body) {
    auto resp = drogon::HttpResponse::newHttpResponse();
    resp->setStatusCode(code);
    // Route through the dedicated content-type setter, NOT addHeader: Drogon
    // serializes Content-Type from an internal field, so addHeader would leave
    // the newHttpResponse() default (text/html) in place AND emit a second
    // Content-Type on the wire. Same pattern HttpClient uses for requests.
    resp->setContentTypeString("application/json");
    resp->setBody(std::string{body});
    return resp;
}

// Canonical domain-error → HTTP-status mapping for the dashboard/login REST
// edges. UiController used to collapse every failing Result to 500, discarding
// the 404/409/422/429/502/504 the domain already carries; this centralizes the
// rule so those statuses survive to the SPA. LoginController's per-endpoint
// mapping (which also picks endpoint-specific error *bodies* and drives the
// reset-grant flow) is consistent with this table. Total over ErrorCode — a
// new enumerator triggers a -Wswitch build error until it is mapped here.
[[nodiscard]] drogon::HttpStatusCode httpStatusForError(aid::plumbing::ErrorCode code) noexcept;

} // namespace aid::controllers
